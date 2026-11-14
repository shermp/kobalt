#pragma once

#include <gio/gio.h>
#include <wpe/webkit.h>

inline constexpr guint maxDimension = 4096;
inline constexpr guint maxFrameBytes = 32 * 1024 * 1024;
inline constexpr char frameFormat[] = "bgra8888-premultiplied";

bool isFrameTmpfs(const char *);
bool validViewport(guint width, guint height);
struct FixturePixels
{
    ~FixturePixels() { g_bytes_unref(bytes); }
    GBytes *bytes;
    guint width, height, stride, page;
    const char *directory;
    bool named = false;
};

FixturePixels *fixturePixels(WebKitImage *, guint width, guint height, unsigned page);
struct FrameBacking
{
    ~FrameBacking();
    GUnixFDList *descriptors = nullptr;
    gchar *path = nullptr;
};

FrameBacking *makeFixtureFrame(const FixturePixels &, GCancellable *);
