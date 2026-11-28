#pragma once

#include <QtCore/QProcess>

#include <array>
#include <fcntl.h>
#include <unistd.h>

class InheritedProcess final : public QProcess
{
public:
    void passDescriptors(std::array<int, 3> descriptors, bool ownGroup = false)
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        inherited = descriptors;
        ownGroup_ = ownGroup;
#else
        setChildProcessModifier([descriptors, ownGroup] { inherit(descriptors, ownGroup); });
#endif
    }

private:
    static void inherit(const std::array<int, 3> &descriptors, bool ownGroup)
    {
        if (ownGroup && setsid() < 0)
            _exit(126);
        for (int fd : descriptors) {
            if (fd < 0)
                continue;
            const int flags = fcntl(fd, F_GETFD);
            if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0)
                _exit(126);
        }
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void setupChildProcess() override { inherit(inherited, ownGroup_); }

    bool ownGroup_ = false;
    std::array<int, 3> inherited{{-1, -1, -1}};
#endif
};
