#include "App.h"
#include "fiber_pool.hpp"
#include "curl_manager.hpp"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <boost/fiber/algo/round_robin.hpp>
#else
#include <fiber.hpp>
#endif

#include <simdjson.h>
#include "agent.hpp"
#include "json_util.hpp"

extern "C" {
struct us_listen_socket_t;
// us_listen_socket_close removed as we no longer listen
}

#ifdef _WIN32
// Boost.Fiber Scheduler for Libuv integration
class uv_scheduler : public boost::fibers::algo::round_robin {
public:
    uv_scheduler(uv_loop_t* loop, uv_async_t* async) : loop_(loop), async_(async) {}

    void suspend_until(std::chrono::steady_clock::time_point const& abs_time) noexcept override {
        if (std::chrono::steady_clock::time_point::max() == abs_time) {
            uv_run(loop_, UV_RUN_ONCE);
        } else {
            uv_run(loop_, UV_RUN_NOWAIT);
        }
        round_robin::suspend_until(abs_time);
    }

    void notify() noexcept override {
        uv_async_send(async_);
        round_robin::notify();
    }

private:
    uv_loop_t* loop_;
    uv_async_t* async_;
};
#endif

thread_local FiberNode* g_current_node = nullptr;

FiberNode* FiberNode::current() {
    return g_current_node;
}

FiberNode::FiberNode(Agent* agent) : agent_(agent) {
    uv_loop_init(&loop_);
    uv_async_init(&loop_, &async_, [](uv_async_t* handle) {
        auto* self = (FiberNode*)handle->data;
        std::unique_lock<std::mutex> lock(self->mtx_);
        while (!self->tasks_.empty()) {
            auto task = std::move(self->tasks_.front());
            self->tasks_.pop();
            lock.unlock();
#ifdef _WIN32
            boost::fibers::fiber(task).detach();
#else
            // In libfiber mode, we use fiber_create/resume.
            // But if we are on the loop thread, and this task is a uWS callback,
            // we should just run it.
            task(); 
#endif
            lock.lock();
        }
    });
    async_.data = this;
}

FiberNode::~FiberNode() {
    stop();
    uv_loop_close(&loop_);
}

void FiberNode::start() {
    running_ = true;
    thread_ = std::thread(&FiberNode::thread_func, this);
}

void FiberNode::stop() {
    if (running_.exchange(false)) {
        uv_async_send(&async_);
#ifdef _WIN32
        spawn([this]() {
            shutdown_cv_.notify_all();
        });
#endif
        if (thread_.joinable()) thread_.join();
    }
}

// Wrapper for the third-party fiber_create entry point
struct fiber_task_wrapper_t {
    std::function<void()> task;
};

// Helper proxy function since third-party fiber expects C-style void*(*)(void*)
static void* fiber_entry_proxy(void* arg) {
    auto* wrapper = static_cast<fiber_task_wrapper_t*>(arg);
    wrapper->task();
    delete wrapper;
    return nullptr;
}

void FiberNode::spawn(std::function<void()> task) {
    if (FiberNode::current() == this) {
        // We are already on the correct thread, spawn immediately
#ifdef _WIN32
        boost::fibers::fiber(task).detach();
#else
        auto* wrapper = new fiber_task_wrapper_t{std::move(task)};
        fiber_t fiber = fiber_create(fiber_entry_proxy, wrapper, nullptr, 1024 * 1024); // 1MB stack
        fiber_resume(fiber);
#endif
    } else {
        // We are on a different thread (e.g. MemoryIndex worker), use async bridge
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.push(std::move(task));
        }
        uv_async_send(&async_);
    }
}

void FiberNode::spawn_back_on_loop(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    uv_async_send(&async_);
}

void FiberNode::thread_func() {
    spdlog::info("Node thread started: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));

    g_current_node = this;

#ifdef _WIN32
    boost::fibers::use_scheduling_algorithm<uv_scheduler>(&loop_, &async_);
#else
    FiberThreadStartup();
#endif

    CurlMultiManager::instance().init(&loop_);
    
    // No longer hosting a server here. 
    // Communication is handled via ChatGateway and WebSocketClient.

#ifdef _WIN32
    boost::fibers::fiber([this]() {
        while (running_.load()) {
            boost::this_fiber::sleep_for(std::chrono::milliseconds(500));
        }
        shutdown_cv_.notify_all();
    }).detach();

    {
        std::unique_lock<boost::fibers::mutex> lock(shutdown_mtx_);
        shutdown_cv_.wait(lock, [this] { return !running_.load(); });
    }
#else
    // Create an idle keepalive fiber so the scheduler doesn't exit
    // (fiber_thread_entry exits when only the scheduler fiber remains)
    auto* idle_wrapper = new fiber_task_wrapper_t{[this]() {
        while (running_.load()) {
            fiber_usleep(500000); // 500ms
        }
    }};
    fiber_t idle_fiber = fiber_create(fiber_entry_proxy, idle_wrapper, nullptr, 0);
    fiber_resume(idle_fiber);

    fibthread_args_t fib_args;
    fib_args.fiberSchedulerCallback = [](void* args) -> bool {
        auto* self = static_cast<FiberNode*>(args);
        uv_run(&self->loop_, UV_RUN_NOWAIT);
        return self->running_.load();
    };
    fib_args.args = this;
    fiber_thread_entry(&fib_args);
#endif
    
    spdlog::info("Node thread exiting");
}

void FiberPool::init(size_t num_threads, Agent* agent) {
    if (num_threads == 0) num_threads = std::thread::hardware_concurrency();
    spdlog::info("Initializing FiberPool with {} nodes", num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        auto node = std::make_unique<FiberNode>(agent);
        node->start();
        nodes_.push_back(std::move(node));
    }
}

void FiberPool::stop() {
    for (auto& node : nodes_) {
        node->stop();
    }
    nodes_.clear();
}

void FiberPool::spawn(std::function<void()> task) {
    if (nodes_.empty()) {
        spdlog::error("FiberPool not initialized!");
        return;
    }
    size_t idx = next_node_.fetch_add(1) % nodes_.size();
    nodes_[idx]->spawn(std::move(task));
}
