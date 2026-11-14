#include "renderer-frame.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace {
// FW4 glibc predates the libc wrapper and these Linux fcntl definitions.
constexpr unsigned memfdFlags = 0x0001 | 0x0002; // CLOEXEC | ALLOW_SEALING
constexpr int addSeals = 1033;
constexpr int immutableSeals = 0x0001 | 0x0002 | 0x0004 | 0x0008;
constexpr long tmpfsMagic = 0x01021994;

bool writeAll(int fd, const unsigned char *data, size_t size)
{
    while (size) {
        const auto written = write(fd, data, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        size -= written;
        data += written;
    }
    return true;
}

bool writeRows(int fd, const unsigned char *pixels, guint stride, guint rowBytes, guint height,
               GCancellable *cancel)
{
    for (guint y = 0; y < height; ++y) {
        if (g_cancellable_is_cancelled(cancel) || !writeAll(fd, pixels + size_t(y) * stride, rowBytes))
            return false;
    }
    return true;
}

int privateFrame(const unsigned char *pixels, guint stride, guint rowBytes, guint height,
                 const char *runDirectory, GCancellable *cancel)
{
#ifdef SYS_memfd_create
    const int memory = syscall(SYS_memfd_create, "kobalt-frame", memfdFlags);
    if (memory >= 0) {
        if (writeRows(memory, pixels, stride, rowBytes, height, cancel)
            && fcntl(memory, addSeals, immutableSeals) == 0)
            return memory;
        close(memory);
    }
#endif
    struct statfs filesystem {};
    if (statfs(runDirectory, &filesystem) != 0 || filesystem.f_type != tmpfsMagic)
        return -1;

    g_autofree gchar *path = g_build_filename(runDirectory, "frame-XXXXXX", nullptr);
    const int writer = g_mkstemp_full(path, O_RDWR | O_CLOEXEC, 0600);
    if (writer < 0)
        return -1;
    int reader = -1;
    if (writeRows(writer, pixels, stride, rowBytes, height, cancel) && fchmod(writer, 0400) == 0)
        reader = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const bool unlinked = unlink(path) == 0;
    close(writer);
    if (!unlinked && reader >= 0) {
        close(reader);
        return -1;
    }
    return reader;
}
} // namespace

bool validViewport(guint width, guint height)
{
    return width && height && width <= maxDimension && height <= maxDimension
            && guint64(width) * height * 4 <= maxFrameBytes;
}

FixturePixels *fixturePixels(WebKitImage *image, guint width, guint height, unsigned page)
{
    static_assert(G_BYTE_ORDER == G_LITTLE_ENDIAN, "Renderer1 pixels require little endian");
    if (!validViewport(width, height) || webkit_image_get_width(image) != int(width)
        || webkit_image_get_height(image) != int(height))
        return nullptr;
    const guint stride = webkit_image_get_stride(image);
    gsize size = 0;
    GBytes *bytes = webkit_image_as_bytes(image);
    if (!bytes)
        return nullptr;
    if (!g_bytes_get_data(bytes, &size) || stride < width * 4 || guint64(stride) * height > size)
        return nullptr;
    return new FixturePixels{ g_bytes_ref(bytes), width, height, stride, page, nullptr };
}

GUnixFDList *makeFixtureFrame(const FixturePixels &frame, GCancellable *cancel)
{
    const auto &[bytes, width, height, stride, page, runDirectory] = frame;
    const auto *pixels = static_cast<const unsigned char *>(g_bytes_get_data(bytes, nullptr));
    const guint rowBytes = width * 4;
    // A fresh fixture view and load event identify the request. Check the pixels as well.
    for (guint y = 0; y < height; ++y) {
        if (g_cancellable_is_cancelled(cancel))
            return nullptr;
        for (guint x = 0; x < width; ++x) {
            const auto *pixel = pixels + size_t(y) * stride + x * 4;
            const bool white = x < 32 && y < 32;
            if (pixel[0] != (white || page == 1 ? 255 : 0) || pixel[1] != (white ? 255 : 0)
                || pixel[2] != (white || page == 0 ? 255 : 0) || pixel[3] != 255)
                return nullptr;
        }
    }

    const int fd = privateFrame(pixels, stride, rowBytes, height, runDirectory, cancel);
    if (fd < 0)
        return nullptr;
    GUnixFDList *list = g_unix_fd_list_new();
    const int handle = g_unix_fd_list_append(list, fd, nullptr);
    close(fd);
    if (handle != 0)
        g_clear_object(&list);
    return list;
}
