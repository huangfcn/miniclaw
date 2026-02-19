#pragma once
#include <uv.h>
#include <fiber.hpp>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <memory>
#include <atomic>
#include "App.h"

// Forward declaration for Agent
class Agent;

class FiberNode {
public:
    FiberNode(Agent* agent);
    ~FiberNode();

    void start();
    void stop();

    // Spawn a task into this node's loop/scheduler
    void spawn(std::function<void()> task);

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
    uWS::App* app_ = nullptr;
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
