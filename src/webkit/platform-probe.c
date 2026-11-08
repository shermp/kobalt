#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Diagnostics only: WebKit's actual sandbox launch remains the final check.
struct Probe
{
    const char *name;
    int required;
    int namespace_flags;
};

struct Result
{
    int error;
    int via_user_namespace;
};

static long long milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int operation(const struct Probe *probe, struct Result *result)
{
    if (probe->namespace_flags) {
        if (!unshare(probe->namespace_flags))
            return 0;
        if ((errno != EPERM && errno != EACCES) || probe->namespace_flags == CLONE_NEWUSER)
            return -1;
        // Bubblewrap may obtain namespace permissions through a user namespace.
        int status = unshare(probe->namespace_flags | CLONE_NEWUSER);
        result->via_user_namespace = !status;
        return status;
    }
    if (!strcmp(probe->name, "subreaper"))
        return prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    if (!strcmp(probe->name, "memfd")) {
        int fd = memfd_create("kobalt-probe", MFD_CLOEXEC);
        if (fd < 0)
            return -1;
        return close(fd);
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        return -1;
    if (!strcmp(probe->name, "no-new-privileges"))
        return 0;
    // Prove that a filter is enforced, rather than just querying the API.
    struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog filter = { sizeof(instructions) / sizeof(instructions[0]), instructions };
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter, 0, 0))
        return -1;
    errno = 0;
    if (syscall(SYS_getppid) == -1 && errno == EACCES)
        return 0;
    errno = EIO;
    return -1;
}

static int run_probe(const struct Probe *probe, struct Result *result)
{
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC))
        return -1;
    long long started = milliseconds();
    if (started < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (!child) {
        close(descriptors[0]);
        struct Result child_result = { 0, 0 };
        if (operation(probe, &child_result))
            child_result.error = errno;
        ssize_t written = write(descriptors[1], &child_result, sizeof(child_result));
        _exit(written == (ssize_t)sizeof(child_result) ? 0 : 2);
    }
    close(descriptors[1]);
    int status = 0, expired = 0;
    for (;;) {
        pid_t reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child)
            break;
        if (reaped < 0 && errno != EINTR) {
            close(descriptors[0]);
            return -1;
        }
        long long now = milliseconds();
        if (now < 0 || now - started >= 2000) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
            expired = 1;
            break;
        }
        struct timespec interval = { 0, 10000000 };
        nanosleep(&interval, NULL);
    }
    ssize_t size = read(descriptors[0], result, sizeof(*result));
    close(descriptors[0]);
    if (expired) {
        result->error = ETIMEDOUT;
        return 0;
    }
    if (size != (ssize_t)sizeof(*result) || !WIFEXITED(status) || WEXITSTATUS(status)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static void json_string(const char *text)
{
    putchar('"');
    for (const unsigned char *at = (const unsigned char *)text; *at; ++at) {
        if (*at == '"' || *at == '\\')
            printf("\\%c", *at);
        else if (*at < 32 || *at >= 127)
            printf("\\u%04x", *at);
        else
            putchar(*at);
    }
    putchar('"');
}

int main(int argc, char **argv)
{
    const char *context = "unspecified";
    if (argc == 3 && !strcmp(argv[1], "--context")
        && (!strcmp(argv[2], "physical-device") || !strcmp(argv[2], "qemu-user")
            || !strcmp(argv[2], "native-host")))
        context = argv[2];
    else if (argc != 1) {
        fprintf(stderr,
                "Usage: kobalt-webkit-probe [--context physical-device|qemu-user|native-host]\n");
        return 2;
    }
    struct utsname system;
    if (uname(&system))
        return 2;
    const struct Probe probes[] = {
        { "subreaper", 1, 0 },
        { "no-new-privileges", 1, 0 },
        { "memfd", 1, 0 },
        { "seccomp-filter", 1, 0 },
        { "user-namespace", 0, CLONE_NEWUSER },
        { "mount-namespace", 1, CLONE_NEWNS },
        { "uts-namespace", 1, CLONE_NEWUTS },
        { "pid-namespace", 1, CLONE_NEWPID },
        { "ipc-namespace", 1, CLONE_NEWIPC },
        { "network-namespace", 1, CLONE_NEWNET },
        { "combined-namespaces", 1,
          CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWNET },
    };
    printf("{\"schema\":1,\"context\":");
    json_string(context);
    printf(",\"kernel_release\":");
    json_string(system.release);
    printf(",\"kernel_machine\":");
    json_string(system.machine);
    printf(",\"euid\":%lu,\"tests\":[", (unsigned long)geteuid());
    int unavailable = 0, failed = 0;
    for (size_t index = 0; index < sizeof(probes) / sizeof(probes[0]); ++index) {
        struct Result result = { 0, 0 };
        int internal = run_probe(&probes[index], &result);
        if (internal)
            result.error = errno;
        failed |= !!internal;
        unavailable |= probes[index].required && result.error;
        printf("%s{\"name\":\"%s\",\"required\":%s,\"result\":\"%s\",\"errno\":%d,\"detail\":",
               index ? "," : "", probes[index].name, probes[index].required ? "true" : "false",
               internal               ? "error"
                       : result.error ? "unavailable"
                                      : "ok",
               result.error);
        json_string(result.error ? strerror(result.error) : "operation succeeded");
        printf(",\"via_user_namespace\":%s}", result.via_user_namespace ? "true" : "false");
    }
    printf("],\"summary\":\"%s\",\"sandbox_verified\":false}\n",
           failed                ? "probe-error"
                   : unavailable ? "unavailable"
                                 : "primitives-available");
    return failed ? 2 : unavailable ? 77 : 0;
}
