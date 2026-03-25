#pragma once
#include <uv.h>
#ifdef _WIN32
#include <boost/fiber/all.hpp>
#else
#include <condition_variable>
#endif
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <memory>
#include <atomic>

// Forward declaration for Agent
class Agent;

class FiberNode {
public:
    FiberNode(Agent* agent);
    ~FiberNode();

    void start();
    void stop();

    // Spawn a task into this node's fiber/thread scheduler
    void spawn(std::function<void()> task);

    // Dispatch a callback back to the loop thread (thread-safe)
    void spawn_back_on_loop(std::function<void()> task);

    static FiberNode* current();
    uv_loop_t* loop() { return &loop_; }

private:
    void thread_func();

    uv_loop_t loop_;
    uv_async_t async_;
    std::thread thread_;
    std::mutex mtx_;
    std::queue<std::function<void()>> tasks_;
    std::atomic<bool> running_{false};

    Agent* agent_;
    void* app_ = nullptr;
    struct us_listen_socket_t *listen_socket_ = nullptr;

#ifdef _WIN32
    boost::fibers::mutex shutdown_mtx_;
    boost::fibers::condition_variable shutdown_cv_;
#else
    std::mutex shutdown_mtx_;
    std::condition_variable shutdown_cv_;
#endif
};

class FiberPool {
public:
    static FiberPool& instance() {
        static FiberPool inst;
        return inst;
    }

    void init(size_t num_threads, Agent* agent);
    void stop();

    // Round-robin dispatch
    void spawn(std::function<void()> task);

private:
    FiberPool() = default;
    std::vector<std::unique_ptr<FiberNode>> nodes_;
    std::atomic<size_t> next_node_{0};
};
