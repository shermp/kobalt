#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int main(void)
{
    struct rlimit core_limit;
    if (getrlimit(RLIMIT_CORE, &core_limit) || core_limit.rlim_cur || core_limit.rlim_max)
        return 17;
    const char *root = getenv("KOBALT_WEBKIT_BUNDLE"), *runtime = getenv("KOBALT_WEBKIT_RUNTIME");
    if (!root || !runtime || getenv("KOBALT_TEST_POISON") || getenv("LD_PRELOAD")
        || getenv("DBUS_SESSION_BUS_ADDRESS") || getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))
        return 10;
    if (fcntl(9, F_GETFD) != -1 || errno != EBADF)
        return 15;
    printf("RUNTIME %s\n", runtime);
    char path[4096], mode[32];
    snprintf(path, sizeof(path), "%s/mode", root);
    FILE *file = fopen(path, "r");
    if (!file || !fgets(mode, sizeof(mode), file))
        return 11;
    fclose(file);
    char link_path[4096], target[4096];
    snprintf(link_path, sizeof(link_path), "%s/escape", runtime);
    snprintf(target, sizeof(target), "%s/keep", root);
    if (symlink(target, link_path))
        return 16;
    const char *disabled = getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    if (!strcmp(mode, "experiment"))
        return disabled && !strcmp(disabled, "1") && getenv("KOBALT_WEBKIT_EXPERIMENT") ? 0 : 12;
    if (disabled || getenv("KOBALT_WEBKIT_EXPERIMENT"))
        return 13;
    pid_t child = fork();
    if (child < 0)
        return 14;
    if (!child) {
        setsid();
        signal(SIGTERM, SIG_IGN);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        for (;;)
            pause();
    }
    printf("CHILD %ld\n", (long)child);
    fflush(stdout);
    if (!strcmp(mode, "crash"))
        raise(SIGKILL);
    if (!strcmp(mode, "hang")) {
        for (;;)
            pause();
    }
    return !strcmp(mode, "failure") ? 23 : 0;
}
