#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <fiber.hpp>
#include <uv.h>

struct TestData {
    int counter = 0;
    fiber_t fiber;
    uv_async_t async;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
};

void* fiber_func(void* arg) {
    TestData* data = (TestData*)arg;
    std::cout << "[Fiber] Started" << std::endl;
    
    for (int i = 0; i < 5; ++i) {
        std::cout << "[Fiber] Suspending " << i << std::endl;
        fiber_suspend(0);
        std::cout << "[Fiber] Resumed " << i << ", counter=" << data->counter << std::endl;
    }
    
    std::cout << "[Fiber] Custom done" << std::endl;
    {
        std::lock_guard<std::mutex> lock(data->mtx);
        data->done = true;
    }
    data->cv.notify_one();
    return nullptr;
}

int main() {
    std::cout << "Starting Fiber + Libuv Test" << std::endl;
    
    FiberGlobalStartup();
    
    uv_loop_t* loop = uv_default_loop();
    
    static auto uv_bridge = [](void* arg) -> bool {
        uv_run((uv_loop_t*)arg, UV_RUN_NOWAIT);
        return true;
    };
    
    static fibthread_args_t fiber_args;
    fiber_args.fiberSchedulerCallback = uv_bridge;
    fiber_args.args = loop;
    
    std::thread sched_thread([&]() {
        FiberThreadStartup();
        fiber_thread_entry(&fiber_args);
    });
    sched_thread.detach();
    
    TestData data;
    
    uv_async_init(loop, &data.async, [](uv_async_t* handle) {
        TestData* d = (TestData*)handle->data;
        std::cout << "[Bridge] Async callback triggered, fiber=" << d->fiber << std::endl;
        if (d->fiber == nullptr) {
            std::cout << "[Bridge] Creating fiber" << std::endl;
            d->fiber = fiber_create(fiber_func, d, NULL, 0);
        } else {
            std::cout << "[Bridge] Resuming fiber" << std::endl;
            fiber_resume(d->fiber);
        }
    });
    data.async.data = &data;
    data.fiber = nullptr;
    
    // Background thread to trigger pulses
    std::thread background([&]() {
        // First pulse to create
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "[Background] Pulsing to create" << std::endl;
        uv_async_send(&data.async);

        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            data.counter++;
            std::cout << "[Background] Pulsing async " << i << std::endl;
            uv_async_send(&data.async);
        }
    });
    background.detach();
    
    // Wait for fiber to finish
    std::unique_lock<std::mutex> lock(data.mtx);
    data.cv.wait(lock, [&]{ return data.done; });
    
    std::cout << "Test Finished Successfully!" << std::endl;
    
    return 0;
}
