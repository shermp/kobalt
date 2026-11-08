#include <cerrno>
#include <cstdio>
#include <cstring>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// Probe the kernel APIs needed by Bubblewrap in disposable child processes.
// An allow-only additional filter cannot relax any inherited seccomp filter.
static int probe(const char *name, int (*operation)())
{
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        errno = 0;
        const int result = operation();
        const int error = errno;
        std::printf("%s result=%d errno=%d (%s)\n", name, result, error, std::strerror(error));
        std::fflush(nullptr);
        _exit(result == 0 ? 0 : 1);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status))
        return 1;
    return WEXITSTATUS(status);
}

int main()
{
    int failures = probe("unshare_user", [] { return unshare(CLONE_NEWUSER); });
    failures += probe("unshare_user_mount", [] { return unshare(CLONE_NEWUSER | CLONE_NEWNS); });
    failures += probe("prctl_no_new_privs", [] { return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); });
    failures += probe("prctl_seccomp_filter", [] {
        struct sock_filter instruction = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        struct sock_fprog program = { 1, &instruction };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
            return -1;
        return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
    });
    failures += probe("syscall_seccomp_filter", [] {
        struct sock_filter instruction = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        struct sock_fprog program = { 1, &instruction };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
            return -1;
        return static_cast<int>(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program));
    });
    return failures ? 1 : 0;
}
