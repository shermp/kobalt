#include <errno.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--child") == 0)
        return 0;

    struct utsname system;
    if (uname(&system) < 0) {
        perror("uname");
        return 1;
    }
    printf("glibc=%s uname=%s %s\n", gnu_get_libc_version(), system.machine, system.release);

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        perror("socketpair");
        return 1;
    }
    close(pair[0]);
    close(pair[1]);

    char name[64];
    snprintf(name, sizeof name, "/kobalt-wpe-m0-%ld", (long)getpid());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return 1;
    }
    if (shm_unlink(name) < 0 || ftruncate(fd, 4096) < 0) {
        perror("shm setup");
        close(fd);
        return 1;
    }
    char *bytes = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (bytes == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    bytes[0] = 'W';
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        if (bytes[0] != 'W')
            _exit(1);
        execl(argv[0], argv[0], "--child", (char *)NULL);
        perror("exec");
        _exit(1);
    }
    int status;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fputs("fork/exec child failed\n", stderr);
        return 1;
    }
    munmap(bytes, 4096);
    puts("PASS AF_UNIX, POSIX shm/mmap and ARM fork/exec");
    return 0;
}
