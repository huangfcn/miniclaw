#pragma once
#include "fiber.h"
#include <chrono>
#include <functional>

namespace fib {
    class Fiber {
    public:
        template<typename Func>
        Fiber(Func&& func, void* stackaddr = nullptr, uint32_t stacksize = 0) {
            boost::fibers::promise<fiber_t> p;
            auto f = p.get_future();
            boost::fibers::fiber([f = std::forward<Func>(func), p = std::make_shared<boost::fibers::promise<fiber_t>>(std::move(p))]() mutable {
                p->set_value(boost::fibers::context::active());
                f();
            }).detach();
            tcb_ = f.get();
        }

        Fiber(fiber_t tcb) : tcb_(tcb) {}

        static inline Fiber* self() { return new Fiber(fiber_ident()); } 
        static inline void usleep(std::chrono::microseconds d) { fiber_usleep(d.count()); }
        static inline void yield() { boost::this_fiber::yield(); }
        static inline void suspend() { fiber_suspend(0); }

        inline bool setLocalData(int index, uint64_t data) {
            fiber_set_localdata(tcb_, index, data);
            return true;
        }

        inline uint64_t getLocalData(int index) {
            return fiber_get_localdata(tcb_, index);
        }

    private:
        fiber_t tcb_;
    };
}
