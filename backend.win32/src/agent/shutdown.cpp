#include "shutdown.hpp"
#include <mutex>
#include <condition_variable>

static std::mutex mtx;
static std::condition_variable cv;
static bool exit_signal = false;

void miniclaw_trigger_shutdown() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        exit_signal = true;
    }
    cv.notify_all();
}

void miniclaw_wait_for_shutdown() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, []{ return exit_signal; });
}
