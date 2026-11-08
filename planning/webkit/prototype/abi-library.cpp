#include <condition_variable>
#include <mutex>
#include <thread>

// Exercise the modern C++ runtime across a private shared-library boundary.
extern "C" int probe_library()
{
    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
    std::thread worker([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        condition.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready; });
    }
    worker.join();
    return 42;
}
