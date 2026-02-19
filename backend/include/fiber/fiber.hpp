#ifndef __LIBFIB_FIBER_HPP__
#define __LIBFIB_FIBER_HPP__

#include "fiber.h"
#include <chrono>
#include <utility> // For std::move and std::forward

namespace fib {

    // Forward declarations
    class Mutex;
    class ConditionVariable;
    class Event;

    // Base class to enable type erasure for the Fiber constructor's callable
    class FiberFuncBase {
    public:
        virtual ~FiberFuncBase() = default;
        virtual void* operator()() = 0;
    };

    // Templated implementation for the fiber's function
    template<typename F>
    class FiberFunc : public FiberFuncBase {
    public:
        FiberFunc(F&& f) : func_(std::forward<F>(f)) {}
        void* operator()() override { return func_(); }
    private:
        F func_;
    };

    class Fiber {
    public:
        // Template constructor to accept any callable, removing std::function
        template<typename Func>
        Fiber(Func&& func, void* stackaddr = nullptr, uint32_t stacksize = 0) {
            // Use dynamic allocation for the function wrapper to keep Fiber's size constant
            // and allow for different callable sizes.
            func_wrapper_ = new FiberFunc<Func>(std::forward<Func>(func));
            tcb_ = fiber_create(entry_point, this, stackaddr, stacksize);
        }

        ~Fiber() {
            delete func_wrapper_;
            // Note: The TCB is managed by the fiber scheduler, not deleted here.
        }

        // Fibers are non-copyable and non-movable to prevent ownership issues.
        Fiber(const Fiber&) = delete;
        Fiber& operator=(const Fiber&) = delete;
        Fiber(Fiber&&) = delete;
        Fiber& operator=(Fiber&&) = delete;

        inline uint64_t resume() {
            return fiber_resume(tcb_);
        }

        static inline Fiber* suspend(uint64_t code = 0) {
            return reinterpret_cast<Fiber*>(fiber_suspend(code));
        }

        static inline Fiber* yield(uint64_t code = 0) {
            return reinterpret_cast<Fiber*>(fiber_yield(code));
        }

        static inline Fiber* self() {
            return reinterpret_cast<Fiber*>(fiber_ident());
        }

        static inline void usleep(std::chrono::microseconds duration) {
            fiber_usleep(duration.count());
        }

        inline bool setLocalData(int index, uint64_t data) {
            return fiber_set_localdata(tcb_, index, data);
        }

        [[nodiscard]] inline uint64_t getLocalData(int index) {
            return fiber_get_localdata(tcb_, index);
        }

        inline bool setCallback(bool (*onTaskStartup)(FibTCB *), bool (*onTaskCleanup)(FibTCB *)) {
            return fiber_install_callbacks(tcb_, onTaskStartup, onTaskCleanup);
        }

    private:
        static void* entry_point(void* arg) {
            Fiber* self = static_cast<Fiber*>(arg);
            return (*self->func_wrapper_)();
        }

        FibTCB* tcb_;
        FiberFuncBase* func_wrapper_;

        friend class Event;
    };

#if defined(__M_N_MODEL__)
    class Mutex {
    public:
        Mutex() { fiber_mutex_init(&mutex_); }
        ~Mutex() { fiber_mutex_destroy(&mutex_); }

        inline void lock() { fiber_mutex_lock(&mutex_); }
        inline void unlock() { fiber_mutex_unlock(&mutex_); }

    private:
        friend class ConditionVariable;
        FibMutex mutex_;
    };
#else // 1:N Model - simplified mutex
    class Mutex {
    public:
        Mutex() { fiber_mutex_init(nullptr); }
        ~Mutex() { fiber_mutex_destroy(nullptr); }
        inline void lock() { fiber_mutex_lock(nullptr); }
        inline void unlock() { fiber_mutex_unlock(nullptr); }
    private:
        friend class ConditionVariable;
    };
#endif

    class Semaphore {
    public:
        explicit Semaphore(int initval = 0) { fiber_sem_init(&sem_, initval); }
        ~Semaphore() { fiber_sem_destroy(&sem_); }

        inline void wait() { fiber_sem_wait(&sem_); }
        [[nodiscard]] inline bool timed_wait(std::chrono::microseconds timeout) {
            return fiber_sem_timedwait(&sem_, timeout.count());
        }
        inline void post() { fiber_sem_post(&sem_); }

    private:
        FibSemaphore sem_;
    };

    class ConditionVariable {
    public:
        ConditionVariable() { fiber_cond_init(&cond_); }
        ~ConditionVariable() { fiber_cond_destroy(&cond_); }

        inline void wait(Mutex& mutex) {
#if defined(__M_N_MODEL__)
            fiber_cond_wait(&cond_, &mutex.mutex_);
#else
            fiber_cond_wait(&cond_, nullptr);
#endif
        }

        [[nodiscard]] inline bool timed_wait(Mutex& mutex, std::chrono::microseconds timeout) {
#if defined(__M_N_MODEL__)
            return fiber_cond_timedwait(&cond_, &mutex.mutex_, timeout.count());
#else
            return fiber_cond_timedwait(&cond_, nullptr, timeout.count());
#endif
        }

        inline void signal() { fiber_cond_signal(&cond_); }
        inline void broadcast() { fiber_cond_broadcast(&cond_); }

    private:
        FibCondition cond_;
    };

    class Event {
    public:
        [[nodiscard]] static inline uint64_t wait(uint64_t events_in, int options, std::chrono::microseconds timeout) {
            return fiber_event_wait(events_in, options, timeout.count());
        }

        static inline int post(Fiber& target_fiber, uint64_t events_in) {
            return fiber_event_post(target_fiber.tcb_, events_in);
        }
    };

    class Scheduler {
    public:
        Scheduler() {
            FiberThreadStartup();
        }

        void run(void* args = nullptr) {
            fiber_thread_entry(args);
        }
    };

} // namespace fib

#endif // __LIBFIB_FIBER_HPP__
