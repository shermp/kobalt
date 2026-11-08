#include <cerrno>
#include <cstdio>
#include <cstring>
#include <gnu/libc-version.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int probe_library();

// ABI/IPC smoke probe only; this does not test WebKit or its sandbox.
int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--child") == 0) {
        std::puts("ARM_CHILD_OK");
        return 0;
    }
    std::printf("glibc=%s private_library=%d\n", gnu_get_libc_version(), probe_library());
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
        return 1;
    const char byte = 'K';
    char received = 0;
    if (write(sockets[0], &byte, 1) != 1 || read(sockets[1], &received, 1) != 1 || received != byte)
        return 2;
    close(sockets[0]);
    close(sockets[1]);
    void *shared = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        return 3;
    *static_cast<int *>(shared) = 0;
    const pid_t child = fork();
    if (child == 0) {
        *static_cast<int *>(shared) = 42;
        _exit(0);
    }
    if (child < 0)
        return 4;
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)
        || *static_cast<int *>(shared) != 42)
        return 5;
    munmap(shared, 4096);
    std::puts("UNIX_IPC_SHARED_MEMORY_OK");
    std::fflush(nullptr);
    const pid_t executableChild = fork();
    if (executableChild == 0) {
        execl(argv[0], argv[0], "--child", static_cast<char *>(nullptr));
        const int error = errno;
        std::fprintf(stderr, "ARM_CHILD_EXEC_FAILED errno=%d (%s)\n", error, std::strerror(error));
        _exit(77);
    }
    if (executableChild < 0 || waitpid(executableChild, &status, 0) != executableChild)
        return 6;
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        return 77;
    return 0;
}
