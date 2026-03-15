#include "fiber_pool.hpp"
#include "App.h"
#include <spdlog/spdlog.h>
#include <simdjson.h>
#include "agent.hpp"
#include "json_util.hpp"

FiberNode::FiberNode(Agent* agent) : agent_(agent) {
    uv_loop_init(&loop_);
    uv_async_init(&loop_, &async_, [](uv_async_t* handle) {
        auto* self = (FiberNode*)handle->data;
        std::unique_lock<std::mutex> lock(self->mtx_);
        while (!self->tasks_.empty()) {
            auto task = std::move(self->tasks_.front());
            self->tasks_.pop();
            lock.unlock();
            if (task) {
                try {
                    task();
                } catch (const std::exception& e) {
                    spdlog::error("Exception in FiberNode loop task: {}", e.what());
                }
            }
            lock.lock();
        }
    });
    async_.data = this;
}

FiberNode::~FiberNode() {
    stop();
    if (app_) delete (uWS::App*)app_;
    uv_loop_close(&loop_);
}

void FiberNode::start() {
    running_ = true;
    thread_ = std::thread(&FiberNode::thread_func, this);
}

void FiberNode::stop() {
    if (running_) {
        running_ = false;
        uv_stop(&loop_);
        uv_async_send(&async_);
        if (thread_.joinable()) thread_.join();
    }
}

void FiberNode::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    uv_async_send(&async_);
}

void FiberNode::thread_func() {
    spdlog::info("IO thread started: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    
    // Initialize uWebSockets Loop wrapper for this thread
    uWS::Loop::get(&loop_);

    app_ = new uWS::App();
    uWS::App* app = (uWS::App*)app_;

    app->get("/api/health", [](auto *res, auto *req) {
        res->end("OK");
    }).options("/*", [](auto *res, auto *req) {
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
           ->writeHeader("Access-Control-Allow-Headers", "content-type, authorization")
           ->end();
    }).post("/v1/chat/completions", [this](auto *res, auto *req) {
        auto aborted = std::make_shared<bool>(false);
        auto body_buffer = std::make_shared<std::string>();
        res->onAborted([aborted]() { *aborted = true; });

        res->onData([this, res, aborted, body_buffer](std::string_view data, bool last) {
            if (*aborted) return;
            body_buffer->append(data.data(), data.length());
            if (last) {
                std::string body = *body_buffer;
                // Capture current node for callbacks
                FiberNode* current_node = this;
                FiberPool::instance().spawn([this, current_node, res, aborted, body]() {
                    if (*aborted) return;
                    simdjson::dom::parser parser;
                    simdjson::dom::element x;
                    if (parser.parse(body).get(x)) return;
                    
                    // Send headers immediately to establish SSE and satisfy CORS
                    FiberPool::instance().post_to_io(current_node, [res, aborted]() {
                        if (*aborted) return;
                        res->writeHeader("Access-Control-Allow-Origin", "*")
                           ->writeHeader("Content-Type", "text/event-stream")
                           ->writeHeader("Cache-Control", "no-cache")
                           ->writeHeader("Connection", "keep-alive");
                    });

                    std::string session_id = "default";
                    std::string_view sid_sv;
                    if (!x["user"].get(sid_sv)) session_id = std::string(sid_sv); // OpenAI uses 'user' for persistent IDs usually

                    std::string message = "";
                    simdjson::dom::array messages_arr;
                    if (!x["messages"].get(messages_arr) && messages_arr.size() > 0) {
                        simdjson::dom::element last_msg;
                        if (!messages_arr.at(messages_arr.size() - 1).get(last_msg)) {
                            std::string_view content_sv;
                            if (!last_msg["content"].get(content_sv)) message = std::string(content_sv);
                        }
                    }

                    std::string chat_id = "chatcmpl-" + std::to_string(std::time(nullptr));
                    this->agent_->run(message, session_id, [current_node, res, aborted, chat_id](const AgentEvent& ev) {
                        if (*aborted) return;
                        FiberPool::instance().post_to_io(current_node, [res, aborted, chat_id, ev]() {
                            if (*aborted) return;
                            if (ev.type == "token") {
                                std::string chunk = "{\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\",\"created\":" + 
                                    std::to_string(std::time(nullptr)) + ",\"model\":\"miniclaw\",\"choices\":[{"
                                    "\"index\":0,\"delta\":{\"content\":\"" + json_util::escape(ev.content) + "\"},\"finish_reason\":null}]}";
                                res->write("data: " + chunk + "\n\n");
                            } else if (ev.type == "tool_start") {
                                // Keep-alive chunk using reasoning_content (OpenAI style)
                                std::string chunk = "{\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\",\"created\":" + 
                                    std::to_string(std::time(nullptr)) + ",\"model\":\"miniclaw\",\"choices\":[{"
                                    "\"index\":0,\"delta\":{\"reasoning_content\":\"[Thinking: " + json_util::escape(ev.content) + "...]\\n\"},\"finish_reason\":null}]}";
                                res->write("data: " + chunk + "\n\n");
                            } else if (ev.type == "done") {
                                res->write("data: [DONE]\n\n");
                                res->end();
                            } else if (ev.type == "error") {
                                std::string chunk = "{\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\",\"created\":" + 
                                    std::to_string(std::time(nullptr)) + ",\"model\":\"miniclaw\",\"choices\":[{"
                                    "\"index\":0,\"delta\":{\"content\":\"\\nError: " + json_util::escape(ev.content) + "\"},\"finish_reason\":\"stop\"}]}";
                                res->write("data: " + chunk + "\n\n");
                                res->write("data: [DONE]\n\n");
                                res->end();
                            }
                        });
                    });
                });
            }
        });
    }).post("/api/chat", [this](auto *res, auto *req) {
        auto aborted = std::make_shared<bool>(false);
        auto body_buffer = std::make_shared<std::string>();
        res->onAborted([aborted]() { *aborted = true; });

        res->onData([this, res, aborted, body_buffer](std::string_view data, bool last) {
            if (*aborted) return;
            body_buffer->append(data.data(), data.length());
            if (last) {
                std::string body = *body_buffer;
                FiberNode* current_node = this;
                FiberPool::instance().spawn([this, current_node, res, aborted, body]() {
                    if (*aborted) return;
                    simdjson::dom::parser parser;
                    simdjson::dom::element x;
                    if (parser.parse(body).get(x)) return;

                    FiberPool::instance().post_to_io(current_node, [res, aborted]() {
                        if (*aborted) return;
                        res->writeHeader("Access-Control-Allow-Origin", "*")
                           ->writeHeader("Content-Type", "text/event-stream")
                           ->writeHeader("Cache-Control", "no-cache")
                           ->writeHeader("Connection", "keep-alive");
                    });

                    std::string message = "", session_id = "default";
                    std::string_view message_sv, session_id_sv;
                    if (!x["message"].get(message_sv)) message = std::string(message_sv);
                    if (!x["session_id"].get(session_id_sv)) session_id = std::string(session_id_sv);
                    
                    this->agent_->run(message, session_id, [current_node, res, aborted](const AgentEvent& ev) {
                        if (*aborted) return;
                        FiberPool::instance().post_to_io(current_node, [res, aborted, ev]() {
                            if (*aborted) return;
                            // Stream ALL events to frontend for better UX
                            std::string chunk = "data: {\"type\":\"" + json_util::escape(ev.type) + "\",\"content\":\"" + json_util::escape(ev.content) + "\"}\n\n";
                            res->write(chunk);
                            if (ev.type == "done" || ev.type == "error") res->end();
                        });
                    });
                });
            }
        });
    });

    app->listen(9000, LIBUS_LISTEN_DEFAULT, [](auto *listen_socket) {
        if (listen_socket) spdlog::info("Worker listening on port 9000");
        else spdlog::error("IO thread failed to listen on port 9000");
    });

    uv_run(&loop_, UV_RUN_DEFAULT);
    spdlog::info("IO thread exiting");
}

void FiberPool::init(size_t num_threads, Agent* agent) {
    // 1. Initialize IO Nodes (fixed small number for responsiveness)
    size_t num_io = std::max((size_t)1, (size_t)(num_threads / 4));
    spdlog::info("Initializing FiberPool with {} IO nodes", num_io);
    for (size_t i = 0; i < num_io; ++i) {
        auto node = std::make_unique<FiberNode>(agent);
        node->start();
        io_nodes_.push_back(std::move(node));
    }

    // 2. Initialize Worker Threads (the rest for compute)
    size_t num_workers = std::max((size_t)1, num_threads);
    spdlog::info("Initializing {} worker threads", num_workers);
    workers_running_ = true;
    for (size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back(&FiberPool::worker_func, this);
    }
}

void FiberPool::stop() {
    {
        std::lock_guard<std::mutex> lock(worker_mtx_);
        workers_running_ = false;
    }
    worker_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();

    for (auto& node : io_nodes_) node->stop();
    io_nodes_.clear();
}

void FiberPool::spawn(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(worker_mtx_);
        worker_tasks_.push(std::move(task));
    }
    worker_cv_.notify_one();
}

void FiberPool::post_to_loop(std::function<void()> task) {
    spawn(std::move(task));
}

void FiberPool::post_to_io(FiberNode* node, std::function<void()> task) {
    if (node) node->post(std::move(task));
}

void FiberPool::worker_func() {
    spdlog::info("Worker thread started");
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(worker_mtx_);
            worker_cv_.wait(lock, [this] { return !workers_running_ || !worker_tasks_.empty(); });
            if (!workers_running_ && worker_tasks_.empty()) break;
            task = std::move(worker_tasks_.front());
            worker_tasks_.pop();
        }
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                spdlog::error("Exception in worker task: {}", e.what());
            }
        }
    }
    spdlog::info("Worker thread exiting");
}
