#include "pool-wire.h"

#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace {
using namespace spike;

class Child
{
public:
    Child(GSocket *socket, GMainLoop *loop, std::array<unsigned char *, 2> maps,
          uint32_t capacity) : socket(socket), loop(loop), maps(maps), capacity(capacity) {}
    int run();

private:
    bool received(const Message &);
    void queue(Type, uint64_t, const std::string &payload = {});
    void paint(uint32_t start, uint32_t end);
    void stop(int code);
    bool receive();
    bool send();

    GSocket *socket;
    GMainLoop *loop;
    GSource *writer = nullptr;
    std::string in, out;
    std::array<unsigned char *, 2> maps;
    uint32_t capacity;
    uint64_t generation = 0;
    FrameSpec frame;
    uint64_t last = 0, pending = 0;
    bool greeted = false, closing = false;
    int status = 0;
};

void Child::stop(int code)
{
    status = code;
    g_main_loop_quit(loop);
}

void Child::queue(Type type, uint64_t id, const std::string &payload)
{
    out += encode(type, id, payload);
    if (writer)
        return;
    writer = g_socket_create_source(socket, G_IO_OUT, nullptr);
    g_source_set_callback(writer, G_SOURCE_FUNC(+[](GSocket *, GIOCondition, gpointer data) {
        return gboolean(static_cast<Child *>(data)->send());
    }), this, nullptr);
    g_source_attach(writer, nullptr);
}

bool Child::send()
{
    g_autoptr(GError) error = nullptr;
    // Bound each send in the spike to exercise incremental framing.
    const auto sent = g_socket_send(socket, out.data(), std::min<size_t>(out.size(), 7),
                                    nullptr, &error);
    if (sent < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        return true;
    if (sent <= 0) {
        stop(1);
        return false;
    }
    out.erase(0, size_t(sent));
    if (!out.empty())
        return true;
    g_clear_pointer(&writer, g_source_unref);
    if (closing)
        stop(0);
    return false;
}

void Child::paint(uint32_t start, uint32_t end)
{
    auto *pixels = maps[frame.slot];
    for (uint32_t i = start; i < end; ++i) {
        pixels[i * 4] = frame.color ? 255 : 0;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = frame.color ? 0 : 255;
        pixels[i * 4 + 3] = 255;
    }
}

bool Child::received(const Message &message)
{
    if (closing || message.id <= last)
        return false;
    last = message.id;
    if (!greeted) {
        if (capacity) {
            if (message.type != PoolHello || message.payload.size() != 12
                || read(message.payload.data() + 8, 4) != capacity)
                return false;
            generation = read(message.payload.data(), 8);
            if (!generation)
                return false;
        } else if (message.type != Hello || !message.payload.empty()) {
            return false;
        }
        greeted = true;
        queue(Ready, message.id, message.payload);
        return true;
    }
    switch (message.type) {
    case Frame:
        if (!capacity || pending || message.payload.size() != 24
            || !FrameSpec::decode(message.payload, frame) || frame.generation != generation
            || !validGeometry(frame.width, frame.height, capacity))
            return false;
        pending = message.id;
        paint(0, capacity / 8);
        // Pause at a deterministic write barrier for cancellation and SIGKILL.
        queue(HalfWritten, pending);
        return true;
    case Finish:
        if (!capacity || !pending || message.payload.size() != 8
            || read(message.payload.data(), 8) != pending)
            return false;
        paint(capacity / 8, capacity / 4);
        queue(FrameReady, pending, frame.reply(capacity));
        pending = 0;
        return true;
    case Work:
        if (capacity || pending || message.payload != std::string(maxPayload, 'w'))
            return false;
        // The mock operation remains pending until Cancel or shutdown.
        pending = message.id;
        queue(Pending, pending);
        return true;
    case Cancel:
        if (!pending || message.payload.size() != 8 || read(message.payload.data(), 8) != pending)
            return false;
        queue(Cancelled, pending);
        pending = 0;
        queue(CancelAck, message.id);
        return true;
    case Shutdown:
        if (pending || !message.payload.empty())
            return false;
        closing = true;
        queue(Bye, message.id);
        return true;
    default:
        return false;
    }
}

bool Child::receive()
{
    char bytes[31];
    g_autoptr(GError) error = nullptr;
    const auto count = g_socket_receive(socket, bytes, sizeof bytes, nullptr, &error);
    if (count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        return true;
    if (count <= 0) {
        stop(count < 0 ? 1 : in.empty() ? 0 : 3);
        return false;
    }
    in.append(bytes, size_t(count));
    Message message{};
    for (;;) {
        switch (take(in, message)) {
        case Parse::Incomplete:
            return true;
        case Parse::Invalid:
            stop(3);
            return false;
        case Parse::Message:
            if (!received(message) || out.size() > maxPayload) {
                stop(3);
                return false;
            }
            break;
        }
    }
}

int Child::run()
{
    auto *reader = g_socket_create_source(socket, G_IO_IN, nullptr);
    g_source_set_callback(reader, G_SOURCE_FUNC(+[](GSocket *, GIOCondition, gpointer data) {
        return gboolean(static_cast<Child *>(data)->receive());
    }), this, nullptr);
    g_source_attach(reader, nullptr);
    g_main_loop_run(loop);
    g_source_destroy(reader);
    g_source_unref(reader);
    if (writer) {
        g_source_destroy(writer);
        g_source_unref(writer);
    }
    return status;
}

int descriptor(const char *text)
{
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    return end != text && !*end && value >= 3 && value <= INT_MAX ? int(value) : -1;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 7)
        return 2;
    const int control = descriptor(argv[1]);
    const int parent = descriptor(argv[2]);
    const int sentinel = descriptor(argv[3]);
    if (control < 0 || parent < 0 || sentinel < 0)
        return 2;
    const int flags = fcntl(control, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) || fcntl(control, F_SETFD, flags | FD_CLOEXEC) < 0
        || fcntl(parent, F_GETFD) != -1 || errno != EBADF
        || fcntl(sentinel, F_GETFD) != -1 || errno != EBADF)
        return 2;
    const int bytes = argc == 7 ? descriptor(argv[6]) : 0;
    std::array<unsigned char *, 2> maps{};
    if (argc == 7) {
        const int a = descriptor(argv[4]), b = descriptor(argv[5]);
        if (bytes <= 0 || unsigned(bytes) > maxFrameBytes || bytes % 4
            || a < 0 || b < 0 || a == b || a == control || b == control)
            return 2;
        const int fds[] = {a, b};
        struct stat objects[2]{};
        for (unsigned i = 0; i < maps.size(); ++i) {
            const int fd = fds[i];
            const int flags = fcntl(fd, F_GETFD);
            auto &info = objects[i];
            if (flags < 0 || (flags & FD_CLOEXEC) || fstat(fd, &info) < 0
                || !S_ISREG(info.st_mode) || info.st_size != bytes || info.st_nlink != 0)
                return 2;
            if (i && info.st_ino == objects[0].st_ino && info.st_dev == objects[0].st_dev)
                return 2;
            void *map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (close(fd) < 0 || map == MAP_FAILED)
                return 2;
            maps[i] = static_cast<unsigned char *>(map);
        }
        for (int fd : fds) {
            if (fcntl(fd, F_GETFD) != -1 || errno != EBADF)
                return 2;
        }
    }
    g_autoptr(GError) error = nullptr;
    g_autoptr(GSocket) socket = g_socket_new_from_fd(control, &error);
    if (!socket || g_socket_get_family(socket) != G_SOCKET_FAMILY_UNIX
        || g_socket_get_socket_type(socket) != G_SOCKET_TYPE_STREAM || !g_socket_is_connected(socket))
        return 2;
    g_socket_set_blocking(socket, FALSE);
    g_autoptr(GMainLoop) loop = g_main_loop_new(nullptr, FALSE);
    const int status = Child(socket, loop, maps, bytes).run();
    for (auto *map : maps) {
        if (map)
            munmap(map, bytes);
    }
    return status;
}
