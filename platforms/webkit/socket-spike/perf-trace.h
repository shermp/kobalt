#pragma once

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Open a separate nonblocking description of the supplied diagnostic pipe.
// Holding both ends prevents SIGPIPE if the collector exits during a render.
inline int openPerfTrace()
{
    struct stat info{};
    if (fstat(STDERR_FILENO, &info) != 0 || !S_ISFIFO(info.st_mode))
        return -1;
    return open("/proc/self/fd/2", O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

template <typename... Args>
void perfTrace(int fd, const char *format, Args... args)
{
    char line[512];
    const int size = std::snprintf(line, sizeof line, format, args...);
    if (size > 0 && size < int(sizeof line))
        (void)write(fd, line, size);
}
