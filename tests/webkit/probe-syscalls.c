#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sched.h>
#include <unistd.h>

// Loaded only by the diagnostic regression test, never by the launcher.
static int unavailable(void)
{
    const char *mode = getenv("KOBALT_TEST_PROBE_FAILURE");
    errno = mode && !strcmp(mode, "permission") ? EPERM : ENOSYS;
    return -1;
}

int unshare(int flags)
{
    (void)flags;
    return unavailable();
}

int memfd_create(const char *name, unsigned flags)
{
    (void)name;
    (void)flags;
    const char *mode = getenv("KOBALT_TEST_PROBE_FAILURE");
    if (mode && !strcmp(mode, "hang")) {
        for (;;)
            pause();
    }
    return unavailable();
}
