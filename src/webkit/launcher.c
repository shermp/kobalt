#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// This executable is deliberately independent of Qt, GLib and the private C++ runtime.
static volatile sig_atomic_t interrupted;

static void on_signal(int number)
{
    interrupted = number;
}

static void fail(const char *message)
{
    fprintf(stderr, "kobalt-webkit: %s: %s\n", message, strerror(errno));
    exit(2);
}

static void environment(const char *name, const char *value)
{
    if (setenv(name, value, 1))
        fail("setenv");
}

static void join_path(char *output, const char *root, const char *suffix)
{
    size_t first = strlen(root), second = strlen(suffix);
    if (first + second >= PATH_MAX) {
        errno = ENAMETOOLONG;
        fail("runtime path");
    }
    memcpy(output, root, first);
    memcpy(output + first, suffix, second + 1);
}

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return remove(path);
}

static long long milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        fail("clock_gettime");
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// A subreaper owns these PIDs even when Bubblewrap creates a separate session.
static void signal_children(int number)
{
    char path[128];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/children", (long)getpid());
    FILE *file = fopen(path, "r");
    if (!file)
        return;
    long pid;
    while (fscanf(file, "%ld", &pid) == 1) {
        if (pid > 1)
            kill((pid_t)pid, number);
    }
    fclose(file);
}

int main(int argc, char **argv)
{
    // Do not keep Nickel's database, device or IPC descriptors alive in this process.
    closefrom(3);
    umask(0077);
    const struct rlimit core_limit = { 0, 0 };
    if (setrlimit(RLIMIT_CORE, &core_limit))
        fail("disable core files");
    const char *bundle_argument = NULL, *guest = NULL;
    const char *sandbox_mode = "required";
    int memory_frames = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--allow-unsandboxed-experiment")
            || !strcmp(argv[i], "--allow-unsandboxed-device")) {
            const char *selected = !strcmp(argv[i], "--allow-unsandboxed-device")
                    ? "device-unsandboxed"
                    : "experiment";
            if (strcmp(sandbox_mode, "required") && strcmp(sandbox_mode, selected)) {
                fprintf(stderr, "kobalt-webkit: conflicting sandbox modes\n");
                return 2;
            }
            sandbox_mode = selected;
        } else if (!strcmp(argv[i], "--memory-frames"))
            memory_frames = 1;
        else if (!strcmp(argv[i], "--bundle") && i + 1 < argc)
            bundle_argument = argv[++i];
        else if (!strcmp(argv[i], "--qemu-prefix") && i + 1 < argc)
            guest = argv[++i];
        else {
            fprintf(stderr,
                    "Usage: kobalt-webkit [--bundle directory] [--qemu-prefix sysroot] "
                    "[--allow-unsandboxed-experiment | --allow-unsandboxed-device] [--memory-frames]\n");
            return 2;
        }
    }
    char executable[PATH_MAX], bundle[PATH_MAX], worker[PATH_MAX], libraries[PATH_MAX * 3];
    if (bundle_argument) {
        if (!realpath(bundle_argument, bundle))
            fail("bundle path");
    } else {
        ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length < 0 || length >= (ssize_t)sizeof(executable) - 1)
            fail("executable path");
        executable[length] = 0;
        char *slash = strrchr(executable, '/');
        if (!slash)
            return 2;
        *slash = 0;
        slash = strrchr(executable, '/');
        if (!slash)
            return 2;
        *slash = 0;
        if (!realpath(executable, bundle))
            fail("bundle path");
    }
    if (strlen(bundle) + 128 >= PATH_MAX)
        return 2;
    join_path(worker, bundle, "/bin/kobalt-webkit-worker");
    if (access(worker, X_OK))
        fail("renderer executable");
    if (guest) {
        if (guest[0] != '/' || strlen(guest) > PATH_MAX - 128)
            return 2;
        snprintf(libraries, sizeof(libraries), "LD_LIBRARY_PATH=%s/private:%s/usr/lib:%s/lib",
                 bundle, guest, guest);
    } else
        snprintf(libraries, sizeof(libraries), "LD_LIBRARY_PATH=%s/private", bundle);

    fprintf(stderr, "kobalt-webkit: sandbox-mode=%s\n", sandbox_mode);
    if (!strcmp(sandbox_mode, "device-unsandboxed"))
        fprintf(stderr, "kobalt-webkit: WebKit sandbox disabled by explicit device opt-in\n");
    char scratch[PATH_MAX];
    snprintf(scratch, sizeof(scratch), "%s/kobalt-webkit-XXXXXX",
             memory_frames ? "/dev/shm" : "/tmp");
    if (memory_frames) {
        struct statfs filesystem;
        if (statfs("/dev/shm", &filesystem) || filesystem.f_type != 0x01021994) {
            fprintf(stderr, "kobalt-webkit: memory frames require tmpfs at /dev/shm\n");
            return 2;
        }
    }
    if (!mkdtemp(scratch))
        fail("runtime directory");
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)) {
        rmdir(scratch);
        fail("child subreaper");
    }
    struct sigaction action = { 0 };
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    pid_t owner = getpid();
    pid_t child = fork();
    if (child < 0) {
        rmdir(scratch);
        fail("fork");
    }
    if (!child) {
        if (setpgid(0, 0) || prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) || getppid() != owner)
            _exit(2);
        if (clearenv())
            _exit(2);
        environment("PATH", "/usr/bin:/bin");
        environment("LANG", "C.UTF-8");
        environment("HOME", scratch);
        environment("TMPDIR", scratch);
        environment("XDG_RUNTIME_DIR", scratch);
        environment("XDG_CACHE_HOME", scratch);
        environment("XDG_CONFIG_HOME", scratch);
        environment("XDG_DATA_HOME", scratch);
        environment("GIO_USE_VFS", "local");
        environment("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
        environment("KOBALT_WEBKIT_BUNDLE", bundle);
        environment("KOBALT_WEBKIT_RUNTIME", scratch);
        if (memory_frames)
            environment("KOBALT_WEBKIT_MEMORY_FRAMES", "1");
        char path[PATH_MAX];
        join_path(path, bundle, "/libexec/wpe-webkit-2.0");
        environment("WEBKIT_EXEC_PATH", path);
        join_path(path, bundle, "/lib/wpe-webkit-2.0/injected-bundle");
        environment("WEBKIT_INJECTED_BUNDLE_PATH", path);
        environment("KOBALT_WEBKIT_SANDBOX_MODE", sandbox_mode);
        if (strcmp(sandbox_mode, "required"))
            environment("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1");
        if (guest) {
            environment("QEMU_LD_PREFIX", guest);
            environment("QEMU_CPU", "cortex-a7");
            execl("/usr/bin/qemu-arm", "qemu-arm", "-cpu", "cortex-a7", "-L", guest, "-E",
                  libraries, worker, (char *)NULL);
        } else {
            environment("LD_LIBRARY_PATH", libraries + strlen("LD_LIBRARY_PATH="));
            execl(worker, worker, (char *)NULL);
        }
        perror("kobalt-webkit: exec renderer");
        _exit(127);
    }
    setpgid(child, child);
    int result = 1, child_exited = 0, terminating = 0;
    long long cleanup_started = 0;
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            if (pid == child) {
                child_exited = 1;
                result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                fprintf(stderr, "kobalt-webkit: renderer exited status=%d\n", result);
            }
            continue;
        }
        if (pid < 0 && errno == ECHILD)
            break;
        if (pid < 0 && errno != EINTR)
            fail("waitpid");
        if ((interrupted || child_exited) && !terminating) {
            terminating = 1;
            cleanup_started = milliseconds();
            if (!child_exited)
                kill(-child, SIGTERM);
        }
        if (terminating) {
            int number = milliseconds() - cleanup_started >= 1000 ? SIGKILL : SIGTERM;
            if (!child_exited)
                kill(-child, number);
            signal_children(number);
            if (milliseconds() - cleanup_started > 5000) {
                fprintf(stderr, "kobalt-webkit: child cleanup deadline expired\n");
                result = 3;
                break;
            }
        }
        struct timespec interval = { 0, 20000000 };
        nanosleep(&interval, NULL);
    }
    if (nftw(scratch, remove_entry, 16, FTW_DEPTH | FTW_PHYS)) {
        fprintf(stderr, "kobalt-webkit: runtime cleanup failed: %s\n", strerror(errno));
        return 3;
    }
    return interrupted ? 128 + interrupted : result;
}
