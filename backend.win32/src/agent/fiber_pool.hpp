#pragma once
#include <uv.h>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <memory>
#include <atomic>
#include <condition_variable>

// Forward declaration for Agent
class Agent;

class FiberNode {
public:
    FiberNode(Agent* agent);
    ~FiberNode();

    void start();
    void stop();

    // Enqueue a task to be run on this node's event loop
    void post(std::function<void()> task);

    uv_loop_t* loop() { return &loop_; }

private:
    void thread_func();

    uv_loop_t loop_;
    uv_async_t async_;
    std::thread thread_;
    std::mutex mtx_;
    std::queue<std::function<void()>> tasks_;
    bool running_ = false;

    Agent* agent_;
    void* app_ = nullptr;
};

class FiberPool {
public:
    static FiberPool& instance() {
        static FiberPool inst;
        return inst;
    }

    void init(size_t num_threads, Agent* agent);
    void stop();

    // Dispatch a task to a generic worker thread (for Agent::run)
    void spawn(std::function<void()> task);

    // Legacy method for backward compatibility, now just calls spawn
    void post_to_loop(std::function<void()> task);

    // Used by worker threads to post callbacks back to the specific IO thread that owns the response
    void post_to_io(FiberNode* node, std::function<void()> task);

private:
    FiberPool() = default;
    
    // IO Nodes (Running uWebSockets)
    std::vector<std::unique_ptr<FiberNode>> io_nodes_;
    
    // Worker Threads (Doing the actual work)
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> worker_tasks_;
    std::mutex worker_mtx_;
    std::condition_variable worker_cv_;
    bool workers_running_ = false;

    void worker_func();
};
