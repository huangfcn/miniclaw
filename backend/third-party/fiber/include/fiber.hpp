#ifndef __LIBFIB_FIBER_HPP__
#define __LIBFIB_FIBER_HPP__

#include "fiber.h"
#include <chrono>
#include <utility> // For std::move and std::forward

namespace fib {

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
        // Template constructor to accept any callable, compatible with original API
        template<typename Func>
        Fiber(Func&& func, void* stackaddr = nullptr, uint32_t stacksize = 0) {
            func_wrapper_ = new FiberFunc<Func>(std::forward<Func>(func));
            tcb_ = fiber_create(entry_point, this, stackaddr, stacksize);
        }

        ~Fiber() {
            delete func_wrapper_;
            // Note: The TCB is managed by the fiber scheduler (GC), not deleted here.
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
            // fiber_suspend returns FibTCB* of the current fiber after resumption.
            // We stored 'this' pointer in the TCB's arg field, but that's used for
            // the entry. To maintain API compatibility, we return nullptr here.
            // The user should use Fiber::self() to get the current fiber.
            fiber_suspend(code);
            return nullptr;
        }

        static inline Fiber* yield(uint64_t code = 0) {
            fiber_yield(code);
            return nullptr;
        }

        static inline Fiber* self() {
            // Not directly supported in lite without extra bookkeeping.
            // Return nullptr; user should use fiber_ident() for the raw TCB.
            return nullptr;
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

        inline FibTCB* tcb() const { return tcb_; }

    private:
        static void* entry_point(void* arg) {
            Fiber* self = static_cast<Fiber*>(arg);
            (*self->func_wrapper_)();
            return nullptr;
        }

        FibTCB* tcb_;
        FiberFuncBase* func_wrapper_;
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
