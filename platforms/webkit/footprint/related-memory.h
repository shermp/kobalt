#pragma once

#include <functional>

namespace related_probe {
enum class Checkpoint : char {
    startup = 1,
    firstLoaded,
    quietBegin,
    quietEnd,
    competingBegin,
    competingEnd,
    secondLoaded,
    thirdLoaded,
    captured,
    navigated,
    reloaded,
    idle,
    released,
    settled
};

// The parent samples out of band, including while the renderer's GLib loop is busy.
int monitor(const std::function<void(int)> &run, bool excludeController = false);
void checkpoint(int socket, Checkpoint);
void requestForeground(int socket);
long long receiveForeground(int socket);
} // namespace related_probe
