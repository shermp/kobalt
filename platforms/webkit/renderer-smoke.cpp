// Standalone service check. This client deliberately uses blocking calls for assertions.
#include <gio/gio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr char path[] = "/org/kobalt/Renderer1";
constexpr char interface[] = "org.kobalt.Renderer1";
constexpr char visualType[] = "(tttta{sv}huuus)";

GVariant *empty()
{
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
}

GVariant *openArgs(guint64 request, guint width, guint height)
{
    return g_variant_new("(tsssuu@a{sv}s)", request, "fixture", "color-pages", "", width, height,
                         empty(), "");
}

GVariant *call(GDBusConnection *peer, const char *method, GVariant *args, const char *type,
               const char *expectedError = nullptr, GUnixFDList **fds = nullptr)
{
    g_autoptr(GError) error = nullptr;
    GVariant *result = g_dbus_connection_call_with_unix_fd_list_sync(
            peer, nullptr, path, interface, method, args, G_VARIANT_TYPE(type),
            G_DBUS_CALL_FLAGS_NONE, 35000, nullptr, fds, nullptr, &error);
    if (expectedError) {
        g_assert_null(result);
        g_assert_nonnull(error);
        g_autofree gchar *name = g_dbus_error_get_remote_error(error);
        g_autofree gchar *expected = g_strconcat(interface, ".Error.", expectedError, nullptr);
        g_assert_cmpstr(name, ==, expected);
    } else {
        g_assert_no_error(error);
        g_assert_nonnull(result);
    }
    return result;
}

struct Mapping
{
    const unsigned char *pixels;
    size_t length;
    ~Mapping() { munmap(const_cast<unsigned char *>(pixels), length); }
};

Mapping frame(GDBusConnection *peer, const char *method, GVariant *args, guint64 request,
              guint64 layout, guint64 revision, guint width, guint height, guint page)
{
    g_autoptr(GUnixFDList) fds = nullptr;
    g_autoptr(GVariant) result = call(peer, method, args, visualType, nullptr, &fds);
    guint64 replyRequest, document, replyLayout, replyFrame;
    guint w, h, stride, actualPage;
    gint handle;
    const char *format;
    g_autoptr(GVariant) state = nullptr;
    g_variant_get(result, "(tttt@a{sv}huuu&s)", &replyRequest, &document, &replyLayout, &replyFrame,
                  &state, &handle, &w, &h, &stride, &format);
    g_assert_cmpuint(replyRequest, ==, request);
    g_assert_cmpuint(document, ==, 1);
    g_assert_cmpuint(replyLayout, ==, layout);
    g_assert_cmpuint(replyFrame, ==, revision);
    g_assert_cmpuint(w, ==, width);
    g_assert_cmpuint(h, ==, height);
    g_assert_cmpuint(stride, ==, width * 4);
    g_assert_cmpstr(format, ==, "bgra8888-premultiplied");
    g_assert_true(g_variant_lookup(state, "pageIndex", "u", &actualPage));
    g_assert_cmpuint(actualPage, ==, page);
    g_assert_nonnull(fds);
    g_assert_cmpint(g_unix_fd_list_get_length(fds), ==, 1);
    g_assert_cmpint(handle, ==, 0);
    const int fd = g_unix_fd_list_get(fds, handle, nullptr);
    g_assert_cmpint(fd, >=, 0);
    struct stat info {};
    g_assert_cmpint(fstat(fd, &info), ==, 0);
    g_assert_true(S_ISREG(info.st_mode));
    const size_t length = size_t(stride) * height;
    g_assert_cmpuint(info.st_size, ==, length);
    const auto *pixels = static_cast<const unsigned char *>(mmap(nullptr, length, PROT_READ,
                                                                 MAP_SHARED, fd, 0));
    g_assert_true(pixels != MAP_FAILED);
    g_assert_cmpint(pwrite(fd, "x", 1, 0), ==, -1);
    close(fd);
    for (guint y = 0; y < height; ++y) {
        for (guint x = 0; x < width; ++x) {
            const auto *pixel = pixels + size_t(y) * stride + x * 4;
            const bool white = x < 32 && y < 32;
            g_assert_cmpuint(pixel[0], ==, white || page == 1 ? 255 : 0);
            g_assert_cmpuint(pixel[1], ==, white ? 255 : 0);
            g_assert_cmpuint(pixel[2], ==, white || page == 0 ? 255 : 0);
            g_assert_cmpuint(pixel[3], ==, 255);
        }
    }
    return { pixels, length };
}

void cancelOpen(GDBusConnection *peer)
{
    bool finished = false;
    g_dbus_connection_call_with_unix_fd_list(
            peer, nullptr, path, interface, "Open", openArgs(2, 1024, 1024),
            G_VARIANT_TYPE(visualType), G_DBUS_CALL_FLAGS_NONE, 35000, nullptr, nullptr,
            +[](GObject *source, GAsyncResult *async, gpointer data) {
                g_autoptr(GError) error = nullptr;
                g_autoptr(GUnixFDList) fds = nullptr;
                g_autoptr(GVariant) result = g_dbus_connection_call_with_unix_fd_list_finish(
                        G_DBUS_CONNECTION(source), &fds, async, &error);
                g_assert_null(result);
                g_assert_null(fds);
                g_assert_nonnull(error);
                g_autofree gchar *name = g_dbus_error_get_remote_error(error);
                g_assert_cmpstr(name, ==, "org.kobalt.Renderer1.Error.Cancelled");
                *static_cast<bool *>(data) = true;
            },
            &finished);
    g_autoptr(GVariant) cancel = call(peer, "Cancel", g_variant_new("(t)", guint64(2)), "(b)");
    gboolean accepted;
    g_variant_get(cancel, "(b)", &accepted);
    g_assert_true(accepted);
    while (!finished)
        g_main_context_iteration(nullptr, TRUE);
}
} // namespace

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 2);
    g_autoptr(GError) error = nullptr;
    g_autoptr(GDBusConnection) peer = g_dbus_connection_new_for_address_sync(
            argv[1], G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, nullptr, nullptr, &error);
    g_assert_no_error(error);
    call(peer, "Close", g_variant_new("(tt)", guint64(1), guint64(1)), "()", "NotReady");
    call(peer, "Hello", g_variant_new("(u)", 2u), "(ua{sv})", "UnsupportedVersion");
    g_autoptr(GVariant) hello = call(peer, "Hello", g_variant_new("(u)", 1u), "(ua{sv})");
    auto red = frame(peer, "Open", openArgs(1, 160, 120), 1, 1, 1, 160, 120, 0);
    cancelOpen(peer);
    auto blue = frame(peer, "Turn", g_variant_new("(ttti)", guint64(3), guint64(1), guint64(1), 1),
                      3, 1, 2, 160, 120, 1);
    call(peer, "Turn", g_variant_new("(ttti)", guint64(4), guint64(1), guint64(1), -1), visualType,
         "StaleState");
    auto again = frame(peer, "Navigate",
                       g_variant_new("(tttss)", guint64(5), guint64(1), guint64(2), "start", ""),
                       5, 1, 3, 160, 120, 0);
    auto resized = frame(peer, "SetViewport",
                         g_variant_new("(tttuu)", guint64(6), guint64(1), guint64(3), 208u, 136u),
                         6, 2, 4, 208, 136, 0);
    auto appearance = frame(peer, "SetAppearance",
                            g_variant_new("(ttt@a{sv})", guint64(7), guint64(1), guint64(4), empty()),
                            7, 2, 5, 208, 136, 0);
    g_autoptr(GVariant) query = call(
            peer, "Query",
            g_variant_new("(ttts@a{sv})", guint64(8), guint64(1), guint64(5), "state", empty()),
            "(tttta{sv})");
    g_autoptr(GVariant) close = call(peer, "Close", g_variant_new("(tt)", guint64(9), guint64(1)),
                                    "()");
    g_autoptr(GVariant) shutdown = call(peer, "Shutdown", nullptr, "()");
    while (!g_dbus_connection_is_closed(peer))
        g_main_context_iteration(nullptr, TRUE);
    g_assert_cmpmem(red.pixels, red.length, again.pixels, again.length);
    g_assert_cmpuint(blue.pixels[(40 * 160 + 40) * 4], ==, 255);
    g_print("Renderer1 peer, cancellation, frame pixels and mapping lifetime passed\n");
}
