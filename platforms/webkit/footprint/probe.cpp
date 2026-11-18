// Standalone reading workload for comparing leased and copied WPE frames.
#include "book-wire.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace spike;
constexpr unsigned width = 1440, height = 1920, bytes = width * height * 4;
constexpr std::array<std::string_view, 2> uris{ "kobalt-epub://book-forma/fixture.xhtml",
                                                "kobalt-epub://book-forma/next.xhtml" };
constexpr std::array<std::string_view, 2> hrefs{ "fixture.xhtml", "next.xhtml" };

void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Process
{
    int pid, parent;
};

std::vector<Process> processes()
{
    std::vector<Process> result;
    auto *directory = opendir("/proc");
    check(directory, "open process table");
    while (auto *entry = readdir(directory)) {
        const int pid = std::atoi(entry->d_name);
        if (!pid)
            continue;
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (!std::getline(stat, line))
            continue;
        std::istringstream fields(line.substr(line.rfind(')') + 1));
        char state;
        int parent;
        if (fields >> state >> parent)
            result.push_back({ pid, parent });
    }
    closedir(directory);
    return result;
}

struct Sample
{
    unsigned long pss = 0, rssHwm = 0;
    int members = 0, fds = 0;
    std::set<std::string> sharedObjects;
};

void includeProcess(Sample &sample, int pid)
{
    const auto root = "/proc/" + std::to_string(pid);
    std::string line;
    std::ifstream smaps(root + "/smaps");
    while (std::getline(smaps, line)) {
        if (line.compare(0, 4, "Pss:") == 0)
            sample.pss += std::strtoul(line.c_str() + 4, nullptr, 10);
        const auto path = line.find('/');
        if (path != std::string::npos && line.find(".so", path) != std::string::npos)
            sample.sharedObjects.insert(line.substr(path));
    }
    std::ifstream status(root + "/status");
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmHWM:") == 0)
            sample.rssHwm += std::strtoul(line.c_str() + 6, nullptr, 10);
    }
    if (auto *directory = opendir((root + "/fd").c_str())) {
        while (auto *entry = readdir(directory))
            sample.fds += entry->d_name[0] != '.';
        closedir(directory);
        if (pid == getpid())
            --sample.fds;
    }
    ++sample.members;
}

void includeTree(Sample &sample, const std::vector<Process> &all, int pid)
{
    includeProcess(sample, pid);
    for (const auto &process : all)
        if (process.parent == pid && process.pid != pid)
            includeTree(sample, all, process.pid);
}

class Renderer
{
public:
    Renderer(const char *path, bool copy) : copyFrames(copy)
    {
        int socket[2];
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, socket) == 0, "socketpair");
        channel = socket[0];
        const timeval timeout{ 18, 0 };
        setsockopt(channel, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        // The unchanged renderer requires two distinct descriptors. Copy mode
        // leaves the second mapping sparse and never sends a write to it.
        for (unsigned i = 0; i < slots.size(); ++i) {
            const auto name =
                    "/kobalt-wpe-m8-" + std::to_string(getpid()) + "-" + std::to_string(i);
            slots[i] = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            check(slots[i] >= 0 && shm_unlink(name.c_str()) == 0 && ftruncate(slots[i], bytes) == 0,
                  "prepare anonymous frame");
            pixels[i] = static_cast<const unsigned char *>(
                    mmap(nullptr, bytes, PROT_READ, MAP_SHARED, slots[i], 0));
            check(pixels[i] != MAP_FAILED, "map frame");
        }
        pid = fork();
        check(pid >= 0, "fork");
        if (!pid) {
            const auto parent = getppid();
            if (setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent)
                _exit(123);
            close(channel);
            for (int fd : slots)
                if (fcntl(fd, F_SETFD, 0) < 0)
                    _exit(125);
            const auto control = std::to_string(socket[1]);
            const auto a = std::to_string(slots[0]), b = std::to_string(slots[1]);
            const auto capacity = std::to_string(bytes);
            execl(path, path, control.c_str(), "80", "90", a.c_str(), b.c_str(), capacity.c_str(),
                  static_cast<char *>(nullptr));
            _exit(124);
        }
        close(socket[1]);
    }

    ~Renderer()
    {
        if (pid > 0) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
        close(channel);
        for (unsigned i = 0; i < 2; ++i) {
            munmap(const_cast<unsigned char *>(pixels[i]), bytes);
            close(slots[i]);
        }
    }

    void start()
    {
        std::string hello;
        put(hello, 57, 8);
        put(hello, bytes, 4);
        const auto id = send(PoolHello, hello);
        const auto ready = receive();
        check(ready.type == Ready && ready.id == id && ready.payload == hello, "renderer hello");
    }

    uint64_t render(unsigned slot, unsigned page, unsigned font = 24, bool preview = false,
                    unsigned spine = 0)
    {
        const auto start = Clock::now();
        BookPage request;
        request.frame = { 57, copyFrames ? 0u : slot, width, height, 0 };
        request.revision = { 57, font, next + 1 };
        request.column = page;
        request.fontPixels = font;
        request.preview = preview;
        request.uri = uris.at(spine);
        request.href = hrefs.at(spine);
        const auto id = send(RenderBookPage, request.encode());
        for (;;) {
            const auto reply = receive();
            if (reply.type == ResourceRequest) {
                resource(reply);
                continue;
            }
            BookFrame frame;
            const auto decoded = frame.decode(reply.payload);
            if (!decoded || (font == 24 && frame.column != page) || frame.columns < 2
                || !(frame.revision == request.revision)
                || reply.payload.substr(0, 36) != request.frame.reply(bytes))
                std::fprintf(
                        stderr,
                        "reply type=%d id=%llu wanted=%llu bytes=%zu decode=%d column=%u columns=%u wantedColumn=%u frameMatch=%d revisionMatch=%d\n",
                        reply.type, (unsigned long long)reply.id, (unsigned long long)id,
                        reply.payload.size(), decoded, frame.column, frame.columns, page,
                        reply.payload.substr(0, 36) == request.frame.reply(bytes),
                        frame.revision == request.revision);
            check(reply.type == BookFrameReady && reply.id == id && decoded
                          && frame.revision == request.revision
                          && (font != 24 || frame.column == page) && frame.columns >= 2
                          && reply.payload.substr(0, 36) == request.frame.reply(bytes),
                  "invalid frame");
            if (copyFrames) {
                auto previous = std::move(images[slot]);
                images[slot].reset(new unsigned char[bytes]);
                std::memcpy(images[slot].get(), pixels[0], bytes);
                lastMs = elapsed(start);
                previous.reset();
                if (!preview)
                    images[1 - slot].reset();
                sample();
            } else {
                lastMs = elapsed(start);
                sample();
            }
            return hash(slot);
        }
    }

    uint64_t hash(unsigned slot) const
    {
        uint64_t result = 1469598103934665603ull;
        const auto *image = copyFrames ? images[slot].get() : pixels[slot];
        for (unsigned i = 0; i < bytes; ++i)
            result = (result ^ image[i]) * 1099511628211ull;
        return result;
    }

    void sample()
    {
        Sample current;
        includeTree(current, processes(), getpid());
        high.pss = std::max(high.pss, current.pss);
        high.rssHwm = std::max(high.rssHwm, current.rssHwm);
        high.members = std::max(high.members, current.members);
        high.fds = std::max(high.fds, current.fds);
        high.sharedObjects.merge(current.sharedObjects);
    }

    double lastMs = 0;
    Sample high;

    void quit()
    {
        const auto id = send(Shutdown, {});
        const auto reply = receive();
        int status;
        check(reply.type == Bye && reply.id == id && waitpid(pid, &status, 0) == pid
                      && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "renderer shutdown");
        check(kill(-pid, 0) == -1 && errno == ESRCH, "renderer process group survived shutdown");
        pid = -1;
    }

private:
    uint64_t send(Type type, const std::string &body)
    {
        const auto wire = encode(type, ++next, body);
        for (size_t sent = 0; sent < wire.size();) {
            const auto count =
                    ::send(channel, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
            check(count > 0, "write");
            sent += size_t(count);
        }
        return next;
    }

    Message receive()
    {
        for (;;) {
            Message message{};
            const auto state = take(input, message);
            check(state != Parse::Invalid, "wire decode");
            if (state == Parse::Message)
                return message;
            char buffer[4096];
            const auto count = recv(channel, buffer, sizeof buffer, 0);
            check(count > 0, "read from renderer");
            input.append(buffer, size_t(count));
        }
    }

    void resource(const Message &message)
    {
        check(message.payload.size() >= 4, "resource request size");
        const auto offset = read(message.payload.data(), 4);
        if (!offset) {
            const auto url = std::string_view(message.payload).substr(4);
            const auto found = std::find(uris.begin(), uris.end(), url);
            serving = found == uris.end() ? nullptr : &documents[found - uris.begin()];
        }
        check(!serving || offset <= serving->size(), "resource offset");
        std::string body;
        put(body, message.id, 8);
        put(body, offset, 4);
        put(body, serving ? serving->size() : 0, 4);
        const std::string media = serving && !offset ? "application/xhtml+xml" : "";
        put(body, media.size(), 1);
        body += media;
        if (serving)
            body.append(*serving, offset,
                        std::min<size_t>(resourceChunk, serving->size() - size_t(offset)));
        send(ResourceData, body);
    }

    static std::string fixture(std::string_view heading)
    {
        std::string book = "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Forma "
                           "probe</title></head><body>";
        for (int chapter = 0; chapter < 3; ++chapter) {
            if (chapter)
                book += "<div style='break-before:column'>";
            else
                book += "<div>";
            book += "<h1>" + std::string(heading) + " " + std::to_string(chapter) + "</h1>";
            for (int row = 0; row < 56; ++row)
                book += "<p>Reading on the Forma: retained frames and source positions.</p>";
            book += "</div>";
        }
        return book + "</body></html>";
    }

    const std::array<std::string, 2> documents{ fixture("Column"), fixture("Next chapter") };
    const bool copyFrames;
    std::array<std::unique_ptr<unsigned char[]>, 2> images;
    int channel = -1;
    pid_t pid = -1;
    uint64_t next = 0;
    const std::string *serving = nullptr;
    std::string input;
    std::array<int, 2> slots{};
    std::array<const unsigned char *, 2> pixels{};
};
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    try {
        // Old kernels lack subreapers. The renderer still owns a process group
        // that quit() checks after reaping the renderer itself.
        check(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0 || errno == EINVAL, "mark probe as subreaper");
        Sample initial;
        includeProcess(initial, getpid());
        std::set<std::string> sharedObjects;
        for (int session = 0; session < 6; ++session) {
            const bool copy = session % 2;
            {
                const auto start = Clock::now();
                Renderer renderer(argv[1], copy);
                renderer.start();
                const double handshake = elapsed(start);
                const auto first = renderer.render(0, 0);
                const double opening = handshake + renderer.lastMs;
                const auto preview = renderer.render(1, 1, 24, true);
                const double previewMs = renderer.lastMs;
                check(first != preview, "preview returned the same pixels as the first page");
                check(renderer.hash(0) == first, "preview overwrote first page");
                check(renderer.render(1, 1) == preview, "next page mismatch");
                std::vector<double> turns;
                for (int turn = 0; turn < 6; ++turn) {
                    check(renderer.render(0, 0) == first, "reverse page mismatch");
                    turns.push_back(renderer.lastMs);
                    check(renderer.render(1, 1) == preview, "forward page mismatch");
                    turns.push_back(renderer.lastMs);
                }
                std::sort(turns.begin(), turns.end());
                const auto nextChapter = renderer.render(0, 0, 24, false, 1);
                const double crossing = renderer.lastMs;
                check(nextChapter != first && nextChapter != preview,
                      "chapter pixels did not change");
                check(renderer.render(1, 0, 30, false, 1) != nextChapter,
                      "font reflow did not change pixels");
                const double reflow = renderer.lastMs;
                check(renderer.high.members == 4, "expected client and three renderer processes");
                renderer.quit();
                std::printf(
                        "mode=%s trial=%d open_ms=%.2f preview_ms=%.2f median_turn_ms=%.2f "
                        "slowest_turn_ms=%.2f chapter_ms=%.2f reflow_ms=%.2f sampled_pss_kib=%lu "
                        "rss_hwm_sum_kib=%lu fds=%d members=%d page0=%016llx page1=%016llx\n",
                        copy ? "one-copy" : "two-lease", session / 2 + 1, opening, previewMs,
                        (turns[5] + turns[6]) / 2, turns.back(), crossing, reflow,
                        renderer.high.pss, renderer.high.rssHwm, renderer.high.fds,
                        renderer.high.members, static_cast<unsigned long long>(first),
                        static_cast<unsigned long long>(preview));
                sharedObjects.merge(renderer.high.sharedObjects);
                std::fflush(stdout);
            }
            Sample after;
            includeTree(after, processes(), getpid());
            check(after.members == 1 && after.fds == initial.fds,
                  "shutdown leaked a process or FD");
        }
        for (const auto &path : sharedObjects)
            if (initial.sharedObjects.count(path) == 0)
                std::printf("mapped=%s\n", path.c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
