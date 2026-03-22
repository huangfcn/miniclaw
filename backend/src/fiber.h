#pragma once
#include <boost/fiber/all.hpp>
#include <chrono>
#include <map>
#include <mutex>
#include <memory>
#include <functional>

#ifdef __cplusplus
extern "C" {
#endif

// Legacy types
typedef boost::fibers::context* fiber_t;
typedef struct FibTCB {
    // Dummy struct to satisfy legacy pointer usage
} FibTCB;

// Fiber-local data storage
struct FiberLocalData {
    std::map<int, uint64_t> locals;
};

inline std::mutex& get_fiber_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline std::map<fiber_t, FiberLocalData>& get_fld_map() {
    static std::map<fiber_t, FiberLocalData> m;
    return m;
}

inline std::map<fiber_t, std::shared_ptr<boost::fibers::promise<void>>>& get_resume_map() {
    static std::map<fiber_t, std::shared_ptr<boost::fibers::promise<void>>> m;
    return m;
}

inline fiber_t fiber_ident() {
    return boost::fibers::context::active();
}

inline void fiber_suspend(uint64_t code) {
    auto ctx = boost::fibers::context::active();
    auto promise = std::make_shared<boost::fibers::promise<void>>();
    {
        std::lock_guard<std::mutex> lock(get_fiber_mutex());
        get_resume_map()[ctx] = promise;
    }
    promise->get_future().wait();
    {
        std::lock_guard<std::mutex> lock(get_fiber_mutex());
        get_resume_map().erase(ctx);
    }
}

inline void fiber_resume(fiber_t fiber) {
    if (!fiber) return;
    std::shared_ptr<boost::fibers::promise<void>> promise;
    {
        std::lock_guard<std::mutex> lock(get_fiber_mutex());
        auto it = get_resume_map().find(fiber);
        if (it != get_resume_map().end()) {
            promise = it->second;
        }
    }
    if (promise) {
        promise->set_value();
    }
}

inline void fiber_usleep(uint64_t usecs) {
    boost::this_fiber::sleep_for(std::chrono::microseconds(usecs));
}

inline void fiber_set_localdata(fiber_t fiber, int index, uint64_t data) {
    if (!fiber) fiber = fiber_ident();
    std::lock_guard<std::mutex> lock(get_fiber_mutex());
    get_fld_map()[fiber].locals[index] = data;
}

inline uint64_t fiber_get_localdata(fiber_t fiber, int index) {
    if (!fiber) fiber = fiber_ident();
    std::lock_guard<std::mutex> lock(get_fiber_mutex());
    auto& m = get_fld_map();
    auto it = m.find(fiber);
    if (it != m.end()) {
        auto it2 = it->second.locals.find(index);
        if (it2 != it->second.locals.end()) return it2->second;
    }
    return 0;
}

inline fiber_t fiber_create(void* (*func)(void*), void* arg, void* stackaddr = nullptr, uint32_t stacksize = 0) {
    auto promise = std::make_shared<boost::fibers::promise<fiber_t>>();
    auto future = promise->get_future();
    boost::fibers::fiber([func, arg, promise]() {
        promise->set_value(boost::fibers::context::active());
        func(arg);
    }).detach();
    return future.get();
}

inline void FiberThreadStartup() {}
inline void FiberGlobalStartup() {}
inline void FiberGlobalCleanup() {}
inline void FiberThreadCleanup() {}

struct fibthread_args_t {
    bool (*fiberSchedulerCallback)(void*);
    void* args;
};

inline void fiber_thread_entry(fibthread_args_t* args) {
    while (true) {
        if (args && args->fiberSchedulerCallback) {
            if (!args->fiberSchedulerCallback(args->args)) break;
        }
        if (boost::fibers::has_ready_fibers()) {
            boost::this_fiber::yield();
        } else {
            // If no fibers are ready, we might want to sleep briefly 
            // but the callback usually handles the I/O wait.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

#ifdef __cplusplus
}
#endif
