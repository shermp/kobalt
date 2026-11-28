#include "related-memory.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <map>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace related_probe {
namespace {
volatile sig_atomic_t interrupted = 0;
constexpr char foregroundEvent = 'F';

long long monotonicUs()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000000LL + time.tv_nsec / 1000;
}

void interrupt(int)
{
    interrupted = 1;
}

void reapGroup(pid_t group)
{
    while (waitpid(-group, nullptr, 0) > 0 || errno == EINTR) { }
}

const char *label(Checkpoint point)
{
    switch (point) {
    case Checkpoint::startup:
        return "startup";
    case Checkpoint::firstLoaded:
        return "first-loaded";
    case Checkpoint::quietBegin:
        return "quiet-begin";
    case Checkpoint::quietEnd:
        return "quiet-end";
    case Checkpoint::competingBegin:
        return "competing-begin";
    case Checkpoint::competingEnd:
        return "competing-end";
    case Checkpoint::secondLoaded:
        return "second-loaded";
    case Checkpoint::thirdLoaded:
        return "third-loaded";
    case Checkpoint::captured:
        return "captured";
    case Checkpoint::navigated:
        return "navigated";
    case Checkpoint::reloaded:
        return "reloaded";
    case Checkpoint::idle:
        return "idle";
    case Checkpoint::released:
        return "released";
    case Checkpoint::settled:
        return "settled";
    }
    return "periodic";
}

using Counters = std::map<std::string, long long>;

Counters readCounters(std::istream &file)
{
    Counters counters;
    for (std::string name; file >> name;) {
        long long value;
        if (file >> value)
            counters[name] += value;
        file.clear();
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return counters;
}

long long counter(const Counters &counters, const char *name)
{
    const auto found = counters.find(name);
    return found != counters.end() ? found->second : -1;
}

struct ProcessMemory
{
    long long pss = -1, rss = -1, dirty = -1, swap = -1;
};

ProcessMemory readMemory(const std::string &path)
{
    std::ifstream file(path + "/smaps_rollup");
    if (!file)
        file.open(path + "/smaps");
    ProcessMemory memory;
    const auto add = [](long long &value, const char *field) {
        value = std::max(0LL, value) + std::strtoll(field, nullptr, 10);
    };
    for (std::string line; std::getline(file, line);) {
        if (line.starts_with("Pss:"))
            add(memory.pss, line.c_str() + 4);
        else if (line.starts_with("Rss:"))
            add(memory.rss, line.c_str() + 4);
        else if (line.starts_with("Private_Dirty:"))
            add(memory.dirty, line.c_str() + 14);
        else if (line.starts_with("Swap:"))
            add(memory.swap, line.c_str() + 5);
    }
    return memory;
}

struct Sample
{
    long long pss = 0, available = -1, cpu = 0;
    int missing = 0, web = 0, network = 0, members = 0;
};

Sample sample(pid_t group, const char *stage, bool excludeController)
{
    const auto now = monotonicUs();
    Sample totals;
    auto *directory = opendir("/proc");
    if (!directory)
        throw std::runtime_error("Open /proc");
    while (auto *entry = readdir(directory)) {
        const int pid = std::atoi(entry->d_name);
        if (!pid || (excludeController && pid == group))
            continue;
        const auto path = "/proc/" + std::to_string(pid);
        std::ifstream stat(path + "/stat");
        std::string line;
        if (!std::getline(stat, line))
            continue;
        const auto end = line.rfind(')');
        const auto begin = line.find('(');
        const auto command = line.substr(begin + 1, end - begin - 1);
        std::istringstream fields(line.substr(end + 2));
        char state;
        int parent, processGroup;
        if (!(fields >> state >> parent >> processGroup) || processGroup != group)
            continue;
        // Fields 14 through 17 count this process and children it has waited for.
        for (int field = 6; field < 14; ++field)
            fields >> line;
        long long user, system, childUser, childSystem;
        if (!(fields >> user >> system >> childUser >> childSystem))
            continue;
        const auto cpu = user + system + childUser + childSystem;
        totals.cpu += cpu;
        // Linux truncates comm to 15 bytes on the Forma.
        totals.web += command.starts_with("WPEWebProcess");
        totals.network += command.starts_with("WPENetworkProce");
        ++totals.members;
        const auto memory = readMemory(path);
        if (state != 'Z') {
            totals.missing += memory.pss < 0;
            totals.pss += std::max(0LL, memory.pss);
        }
        std::printf("process t_us=%lld stage=%s pid=%d ppid=%d comm=%s state=%c pss_kib=%lld "
                    "rss_kib=%lld private_dirty_kib=%lld swap_kib=%lld cpu_ticks=%lld\n",
                    now, stage, pid, parent, command.c_str(), state, memory.pss, memory.rss,
                    memory.dirty, memory.swap, cpu);
    }
    closedir(directory);
    std::ifstream meminfo("/proc/meminfo"), vmstat("/proc/vmstat");
    const auto memory = readCounters(meminfo), vm = readCounters(vmstat);
    totals.available = counter(memory, "MemAvailable:");
    std::printf("sample t_us=%lld stage=%s members=%d web=%d network=%d pss_kib=%lld "
                "missing=%d mem_available_kib=%lld swap_total_kib=%lld swap_free_kib=%lld "
                "pswpin=%lld pswpout=%lld oom_kill=%lld cpu_ticks=%lld\n",
                now, stage, totals.members, totals.web, totals.network, totals.pss, totals.missing,
                totals.available, counter(memory, "SwapTotal:"), counter(memory, "SwapFree:"),
                counter(vm, "pswpin"), counter(vm, "pswpout"), counter(vm, "oom_kill"), totals.cpu);
    return totals;
}
} // namespace

void checkpoint(int socket, Checkpoint point)
{
    auto value = static_cast<char>(point);
    if (send(socket, &value, 1, MSG_NOSIGNAL) != 1 || recv(socket, &value, 1, 0) != 1
        || value != static_cast<char>(point))
        throw std::runtime_error("Memory sampler disconnected");
}

void requestForeground(int socket)
{
    if (send(socket, &foregroundEvent, 1, MSG_NOSIGNAL) != 1)
        throw std::runtime_error("Request foreground turn");
}

long long receiveForeground(int socket)
{
    long long timestamp;
    if (recv(socket, &timestamp, sizeof timestamp, 0) != sizeof timestamp)
        throw std::runtime_error("Read foreground request");
    return timestamp;
}

int monitor(const std::function<void(int)> &run, bool excludeController)
{
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0 && errno != EINVAL)
        throw std::runtime_error("Set probe subreaper");
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0)
        throw std::runtime_error("Create memory sampler socket");
    const auto pid = fork();
    if (pid < 0) {
        close(sockets[0]);
        close(sockets[1]);
        throw std::runtime_error("Fork probe renderer");
    }
    if (!pid) {
        close(sockets[0]);
        if (setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() == 1)
            _exit(2);
        alarm(300);
        try {
            run(sockets[1]);
            _exit(0);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "renderer: %s\n", error.what());
            _exit(1);
        }
    }

    close(sockets[1]);
    struct sigaction action{};
    action.sa_handler = interrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    Sample phase;
    long long peak = 0, minimum = -1, phasePeak = 0;
    int maxWeb = 0, maxNetwork = 0;
    const auto ticksPerSecond = sysconf(_SC_CLK_TCK);
    int status = 0;
    try {
        pollfd fd{ sockets[0], POLLIN, 0 };
        for (;;) {
            if (interrupted)
                throw std::runtime_error("Probe interrupted");
            const auto result = poll(&fd, 1, 100);
            if (result < 0 && errno == EINTR)
                continue;
            if (result < 0)
                throw std::runtime_error("Poll memory sampler");
            char event = 0;
            if (result && recv(fd.fd, &event, 1, 0) != 1)
                break;
            if (event == foregroundEvent) {
                const auto issued = monotonicUs();
                if (send(fd.fd, &issued, sizeof issued, MSG_NOSIGNAL) != sizeof issued)
                    break;
                continue;
            }
            const auto point = static_cast<Checkpoint>(event);
            const auto current = sample(pid, label(point), excludeController);
            if (!current.missing) {
                peak = std::max(peak, current.pss);
                phasePeak = std::max(phasePeak, current.pss);
            }
            if (current.available >= 0)
                minimum = minimum < 0 ? current.available : std::min(minimum, current.available);
            maxWeb = std::max(maxWeb, current.web);
            maxNetwork = std::max(maxNetwork, current.network);
            if (point == Checkpoint::quietBegin || point == Checkpoint::competingBegin) {
                phase = current;
                phasePeak = current.pss;
            }
            if (point == Checkpoint::quietEnd || point == Checkpoint::competingEnd)
                std::printf("phase stage=%s cpu_ms=%.1f peak_pss_kib=%lld\n", label(point),
                            1000.0 * (current.cpu - phase.cpu) / ticksPerSecond, phasePeak);
            if (event && send(fd.fd, &event, 1, MSG_NOSIGNAL) != 1)
                break;
        }
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR || interrupted)
                throw std::runtime_error("Wait for probe renderer");
        }
        const auto after = sample(pid, "shutdown", excludeController);
        std::printf("summary peak_pss_kib=%lld min_mem_available_kib=%lld "
                    "max_web=%d max_network=%d shutdown_members=%d status=%d\n",
                    peak, minimum, maxWeb, maxNetwork, after.members, status);
        if (after.members) {
            kill(-pid, SIGKILL);
            reapGroup(pid);
        }
        close(fd.fd);
        if (!peak || minimum < 0) {
            std::fprintf(stderr, "No complete memory or headroom samples\n");
            return 1;
        }
        return !WIFEXITED(status) || WEXITSTATUS(status) || after.members;
    } catch (...) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        close(sockets[0]);
        waitpid(pid, nullptr, 0);
        reapGroup(pid);
        throw;
    }
}
} // namespace related_probe
