// Standalone reading workload for comparing leased and copied WPE frames.
#include "book-wire.h"
#include "related-memory.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace spike;
constexpr unsigned width = 1440, height = 1920, bytes = width * height * 4;
constexpr std::string_view origin = "kobalt-epub://book-forma/";
constexpr std::array<std::string_view, 5> hrefs{ "fixture.xhtml", "next.xhtml", "styles.css",
                                                 "font.ttf", "ornament.png" };
constexpr std::array<const char *, hrefs.size()> mediaTypes{ "application/xhtml+xml",
                                                             "application/xhtml+xml", "text/css",
                                                             "font/ttf", "image/png" };

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

double childCpuMs()
{
    rusage usage{};
    check(getrusage(RUSAGE_CHILDREN, &usage) == 0, "read child CPU time");
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0
            + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
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
    Renderer(const char *path, bool copy, std::string_view workload, const char *fixtureDir,
             bool sourcePages = false)
        : copyFrames(copy),
          baseline(!workload.empty()),
          cold(workload == "--cold"),
          sourcePages(sourcePages)
    {
        if (cold) {
            std::ifstream manifest(std::string(fixtureDir) + "/resources.txt");
            for (std::string name; manifest >> name;) {
                std::string media;
                check(bool(manifest >> media), "missing fixture resource media type");
                check(name.find_first_not_of(
                              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
                                      == std::string::npos
                              && name != "." && name != "..",
                      "unsafe fixture resource name");
                check(fixtureResources
                              .emplace(name,
                                       Resource{ readFixture(fixtureDir, name), std::move(media) })
                              .second,
                      "duplicate fixture resource");
            }
            check(!fixtureResources.empty() && manifest.eof(), "read cold fixture manifest");
        }
        const bool resources = workload == "resources";
        const int rows = workload == "long" ? 512 : 56;
        documents[0] = fixture("Column", rows, resources);
        documents[1] = fixture("Next chapter", rows, resources);
        if (resources) {
            documents[2] = "@font-face { font-family: Probe; src: url(font.ttf); }"
                           "body, p { font-family: Probe, serif; }";
            documents[3] = readFixture(fixtureDir, hrefs[3]);
            documents[4] = readFixture(fixtureDir, hrefs[4]);
        }
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
            if ((!cold && setsid() < 0) || prctl(PR_SET_PDEATHSIG, SIGKILL) < 0
                || getppid() != parent)
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
        request.href =
                cold ? "chapter" + std::to_string(spine) + ".xhtml" : std::string(hrefs.at(spine));
        request.uri = std::string(origin) + request.href;
        const auto id = send(RenderBookPage, request.encode());
        for (;;) {
            const auto reply = receive();
            if (reply.type == ResourceRequest) {
                resource(reply);
                continue;
            }
            BookFrame frame;
            const auto decoded = frame.decode(reply.payload);
            const bool expectedColumn =
                    frame.column == (page == lastColumn ? frame.columns - 1 : page);
            if (!decoded || (font == 24 && !expectedColumn) || frame.columns < 2
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
                          && frame.revision == request.revision && (font != 24 || expectedColumn)
                          && frame.columns >= 2
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
                if (!cold && (!baseline || preview || page == 0))
                    sample();
            }
            if (baseline)
                std::printf("wpe-perf-probe pid=%d request=%llu roundtrip_ms=%.3f\n", int(pid),
                            static_cast<unsigned long long>(id), lastMs);
            const auto fingerprint = hash(slot);
            lastColumns = frame.columns;
            if (cold) {
                // Exclude the slot and revision header from the page metadata hash.
                uint64_t metadata = 1469598103934665603ull;
                const auto pageMetadata = std::string_view(reply.payload).substr(60);
                for (unsigned char byte : pageMetadata)
                    metadata = (metadata ^ byte) * 1099511628211ull;
                std::printf("cold-frame pid=%d request=%llu spine=%u column=%u columns=%u "
                            "roundtrip_ms=%.3f fingerprint=%016llx metadata=%016llx\n",
                            int(pid), (unsigned long long)id, spine, frame.column, frame.columns,
                            lastMs, (unsigned long long)fingerprint, (unsigned long long)metadata);
                if (sourcePages) {
                    std::fputs("cold-source wire=", stdout);
                    constexpr char hex[] = "0123456789abcdef";
                    for (unsigned char byte : pageMetadata) {
                        std::putchar(hex[byte >> 4]);
                        std::putchar(hex[byte & 15]);
                    }
                    std::putchar('\n');
                }
            }
            return fingerprint;
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
        includeTree(current, processes(), baseline ? pid : getpid());
        high.pss = std::max(high.pss, current.pss);
        high.rssHwm = std::max(high.rssHwm, current.rssHwm);
        high.members = std::max(high.members, current.members);
        high.fds = std::max(high.fds, current.fds);
        high.sharedObjects.merge(current.sharedObjects);
    }

    double lastMs = 0;
    unsigned lastColumns = 0;
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
    struct Resource
    {
        std::string bytes, media;
    };

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
            auto url = std::string_view(message.payload).substr(4);
            serving = nullptr;
            if (url.starts_with(origin)) {
                url.remove_prefix(origin.size());
                if (cold) {
                    const auto found = fixtureResources.find(url);
                    if (found != fixtureResources.end()) {
                        serving = &found->second.bytes;
                        servingMedia = found->second.media;
                    }
                } else {
                    const auto found = std::find(hrefs.begin(), hrefs.end(), url);
                    if (found != hrefs.end() && !documents[found - hrefs.begin()].empty()) {
                        const auto index = found - hrefs.begin();
                        serving = &documents[index];
                        servingMedia = mediaTypes[index];
                    }
                }
            }
        }
        check(!serving || offset <= serving->size(), "resource offset");
        std::string body;
        put(body, message.id, 8);
        put(body, offset, 4);
        put(body, serving ? serving->size() : 0, 4);
        const std::string_view media = serving && !offset ? servingMedia : "";
        put(body, media.size(), 1);
        body += media;
        if (serving)
            body.append(*serving, offset,
                        std::min<size_t>(resourceChunk, serving->size() - size_t(offset)));
        send(ResourceData, body);
    }

    static std::string readFixture(const char *directory, std::string_view name)
    {
        std::ifstream file(std::string(directory) + "/" + std::string(name), std::ios::binary);
        check(file.good(), "open exported fixture resource");
        std::string bytes{ std::istreambuf_iterator<char>(file), {} };
        check(!bytes.empty() && bytes.size() <= maxResourceBytes, "fixture resource size");
        return bytes;
    }

    static std::string fixture(std::string_view heading, int rows, bool resources)
    {
        std::string book = "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Forma "
                           "probe</title>";
        if (resources)
            book += "<link rel='stylesheet' href='styles.css'/>";
        book += "</head><body>";
        for (int chapter = 0; chapter < 3; ++chapter) {
            if (chapter)
                book += "<div style='break-before:column'>";
            else
                book += "<div>";
            book += "<h1>" + std::string(heading) + " " + std::to_string(chapter) + "</h1>";
            if (resources)
                book += "<img src='ornament.png' alt='' />";
            for (int row = 0; row < rows; ++row)
                book += "<p>Reading on the Forma: retained frames and source positions.</p>";
            book += "</div>";
        }
        return book + "</body></html>";
    }

    std::array<std::string, hrefs.size()> documents;
    const bool copyFrames, baseline, cold, sourcePages;
    std::map<std::string, Resource, std::less<>> fixtureResources;
    std::array<std::unique_ptr<unsigned char[]>, 2> images;
    int channel = -1;
    pid_t pid = -1;
    uint64_t next = 0;
    const std::string *serving = nullptr;
    std::string_view servingMedia;
    std::string input;
    std::array<int, 2> slots{};
    std::array<const unsigned char *, 2> pixels{};
};
} // namespace

int main(int argc, char **argv)
{
    const std::string_view workload = argc >= 3 ? argv[2] : "";
    const bool imagePages = argc == 5 && std::string_view(argv[4]) == "--image-pages";
    const bool sourcePages = argc == 5 && std::string_view(argv[4]) == "--source-pages";
    if ((argc == 4 || imagePages || sourcePages) && workload == "--cold") {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        try {
            return related_probe::monitor(
                    [&](int monitor) {
                        using related_probe::Checkpoint;
                        Renderer renderer(argv[1], false, workload, argv[3], sourcePages);
                        renderer.start();
                        related_probe::checkpoint(monitor, Checkpoint::startup);
                        const auto cpu = childCpuMs();
                        unsigned slot = 0;
                        for (unsigned visit = 0; visit < 8; ++visit) {
                            const unsigned spine = visit % 4;
                            const auto initial = sourcePages && visit >= 4 ? lastColumn : 0u;
                            const auto first = renderer.render(slot, initial, 24, false, spine);
                            const unsigned count = renderer.lastColumns;
                            const auto adjacent = initial == lastColumn ? count - 2 : 1u;
                            const auto second =
                                    renderer.render(1 - slot, adjacent, 24, false, spine);
                            check(first != second, "adjacent columns returned identical pixels");
                            if (imagePages) {
                                check(renderer.lastColumns == count,
                                      "second column changed pagination");
                                check(count >= 3, "not enough columns for the image check");
                                renderer.render(1 - slot, count / 2, 24, false, spine);
                                check(renderer.lastColumns == count,
                                      "middle column changed pagination");
                                renderer.render(1 - slot, count - 1, 24, false, spine);
                                check(renderer.lastColumns == count,
                                      "final column changed pagination");
                            }
                            check(renderer.render(slot, initial, 24, false, spine) == first,
                                  "revisiting the first column returned different pixels");
                            slot = 1 - slot;
                        }
                        related_probe::checkpoint(monitor, Checkpoint::navigated);
                        renderer.quit();
                        std::printf("cold-summary child_cpu_ms=%.2f\n", childCpuMs() - cpu);
                    },
                    true);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    if (argc != 2 && !(argc == 3 && (workload == "short" || workload == "long"))
        && !(argc == 4 && workload == "resources")) {
        std::fprintf(
                stderr,
                "usage: %s RENDERER [short|long|resources FIXTURE_DIR|--cold FIXTURE_DIR [--image-pages|--source-pages]]\n",
                argv[0]);
        return 2;
    }
    const bool baseline = !workload.empty();
    try {
        // Old kernels lack subreapers. The renderer still owns a process group
        // that quit() checks after reaping the renderer itself.
        check(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0 || errno == EINVAL, "mark probe as subreaper");
        Sample initial;
        includeProcess(initial, getpid());
        std::set<std::string> sharedObjects;
        for (int session = 0; session < (baseline ? 3 : 6); ++session) {
            const bool copy = !baseline && session % 2;
            {
                const auto start = Clock::now();
                const auto cpuBefore = baseline ? childCpuMs() : 0;
                Renderer renderer(argv[1], copy, workload, argc == 4 ? argv[3] : nullptr);
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
                for (int turn = 0; turn < (baseline ? 10 : 6); ++turn) {
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
                check(renderer.high.members == (baseline ? 3 : 4), "unexpected process count");
                renderer.quit();
                if (baseline)
                    std::printf("workload=%s trial=%d child_cpu_ms=%.2f\n", argv[2], session + 1,
                                childCpuMs() - cpuBefore);
                std::printf(
                        "mode=%s trial=%d open_ms=%.2f preview_ms=%.2f median_turn_ms=%.2f "
                        "slowest_turn_ms=%.2f chapter_ms=%.2f reflow_ms=%.2f sampled_pss_kib=%lu "
                        "rss_hwm_sum_kib=%lu fds=%d members=%d page0=%016llx page1=%016llx\n",
                        copy ? "one-copy" : "two-lease", (baseline ? session : session / 2) + 1,
                        opening, previewMs,
                        (turns[turns.size() / 2 - 1] + turns[turns.size() / 2]) / 2, turns.back(),
                        crossing, reflow, renderer.high.pss, renderer.high.rssHwm,
                        renderer.high.fds, renderer.high.members,
                        static_cast<unsigned long long>(first),
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
