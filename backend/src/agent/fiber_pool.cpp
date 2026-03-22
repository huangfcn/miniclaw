#include "App.h"
#include "fiber_pool.hpp"
#include "curl_manager.hpp"
#include <spdlog/spdlog.h>
#include <boost/fiber/algo/round_robin.hpp>

#include <simdjson.h>
#include "agent.hpp"
#include "json_util.hpp"

extern "C" {
struct us_listen_socket_t;
void us_listen_socket_close(int ssl, struct us_listen_socket_t *ls);
}

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

thread_local FiberNode* g_current_node = nullptr;

FiberNode* FiberNode::current() {
    return g_current_node;
}

FiberNode::FiberNode(Agent* agent) : agent_(agent) {
    uv_loop_init(&loop_);
    uv_async_init(&loop_, &async_, [](uv_async_t* handle) {
        auto* self = (FiberNode*)handle->data;
        if (!self->running_.load() && self->listen_socket_) {
            us_listen_socket_close(0, self->listen_socket_);
            self->listen_socket_ = nullptr;
        }

        std::unique_lock<std::mutex> lock(self->mtx_);
        while (!self->tasks_.empty()) {
            auto task = std::move(self->tasks_.front());
            self->tasks_.pop();
            lock.unlock();
            // In Boost.Fiber, we spawn a fiber for each task
            boost::fibers::fiber(task).detach();
            lock.lock();
        }
    });
    async_.data = this;
}

FiberNode::~FiberNode() {
    stop();
    if (app_) delete static_cast<uWS::App*>(app_);
    uv_loop_close(&loop_);
}

void FiberNode::start() {
    running_ = true;
    thread_ = std::thread(&FiberNode::thread_func, this);
}

void FiberNode::stop() {
    if (running_.exchange(false)) {
        uv_async_send(&async_);
        // Also wake up the main fiber waiting on the CV
        spawn([this]() {
            shutdown_cv_.notify_all();
        });
        if (thread_.joinable()) thread_.join();
    }
}

void FiberNode::spawn(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    uv_async_send(&async_);
}

void FiberNode::thread_func() {
    spdlog::info("Fiber node thread started: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));

    g_current_node = this;

    // Use custom scheduler for this thread
    boost::fibers::use_scheduling_algorithm<uv_scheduler>(&loop_, &async_);

    CurlMultiManager::instance().init(&loop_);
    uWS::Loop::get(&loop_);

    app_ = new uWS::App();

    auto* uws_app = static_cast<uWS::App*>(app_);

    uws_app->get("/api/health", [](auto *res, auto *req) {
        res->end("OK");
    }).options("/*", [](auto *res, auto *req) {
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
           ->writeHeader("Access-Control-Allow-Headers", "content-type, authorization")
           ->end();
    }).post("/api/shutdown", [](auto *res, auto *req) {
        spdlog::info("Shutdown requested via API");
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->end("{\"status\":\"shutting_down\"}");
        miniclaw_trigger_shutdown();
    }).post("/v1/chat/completions", [this](auto *res, auto *req) {
        auto aborted = std::make_shared<bool>(false);
        auto body_buffer = std::make_shared<std::string>();

        res->onAborted([aborted]() {
            *aborted = true;
            spdlog::warn("OpenAI API request aborted");
        });

        std::string auth_header(req->getHeader("authorization"));
        if (auth_header.rfind("Bearer ", 0) == 0) {
            auth_header = auth_header.substr(7);
        }

        res->onData([this, res, aborted, body_buffer, auth_header](std::string_view data, bool last) {
            if (*aborted) return;
            body_buffer->append(data.data(), data.length());
            if (last) {
                simdjson::dom::parser parser;
                simdjson::dom::element x;
                auto error = parser.parse(*body_buffer).get(x);
                if (error) {
                    res->writeStatus("400 Bad Request")->end("Invalid JSON");
                    return;
                }

                std::string message = "";
                std::string_view requested_model_sv;
                if (!x["model"].get(requested_model_sv)) {}
                std::string requested_model = std::string(requested_model_sv.empty() ? "unknown" : requested_model_sv);

                simdjson::dom::array messages;
                if (!x["messages"].get(messages)) {
                    if (messages.size() > 0) {
                        simdjson::dom::element last_msg;
                        if (!messages.at(messages.size() - 1).get(last_msg)) {
                            std::string_view content_sv;
                            if (!last_msg["content"].get(content_sv)) {
                                message = std::string(content_sv);
                            }
                        }
                    }
                }
                
                spdlog::info("OpenAI Chat Completion: model={}, session=default, msg_len={}", requested_model, message.length());
                std::string session_id = "default";

                res->writeStatus("200 OK")
                   ->writeHeader("Content-Type", "text/event-stream")
                   ->writeHeader("Cache-Control", "no-cache")
                   ->writeHeader("Connection", "keep-alive")
                   ->writeHeader("Access-Control-Allow-Origin", "*");

                std::string chat_id = "chatcmpl-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

                // Spawn Boost.Fiber
                boost::fibers::fiber([this, res, aborted, chat_id, message, session_id]() {
                    agent_->run(message, session_id, [res, aborted, chat_id](const AgentEvent& ev) {
                        if (*aborted) return;
                        if (ev.type == "token") {
                            std::string chunk = "{\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\",\"created\":" + 
                                std::to_string(std::time(nullptr)) + ",\"model\":\"miniclaw\",\"choices\":[{"
                                "\"index\":0,\"delta\":{\"content\":\"" + json_util::escape(ev.content) + "\"},\"finish_reason\":null}]}";
                            res->write("data: " + chunk + "\n\n");
                        } else if (ev.type == "done") {
                            std::string chunk = "{\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\",\"created\":" + 
                                std::to_string(std::time(nullptr)) + ",\"model\":\"miniclaw\",\"choices\":[{"
                                "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}";
                            res->write("data: " + chunk + "\n\n");
                            res->write("data: [DONE]\n\n");
                            res->end();
                        } else if (ev.type == "tool_start") {
                            std::string chunk = "{\"id\":\"" + chat_id + "\",\"object\":\"chat.completion.chunk\",\"created\":" + 
                                std::to_string(std::time(nullptr)) + ",\"model\":\"miniclaw\",\"choices\":[{"
                                "\"index\":0,\"delta\":{\"reasoning_content\":\"[Thinking: " + json_util::escape(ev.content) + "...]\\n\"},\"finish_reason\":null}]}";
                            res->write("data: " + chunk + "\n\n");
                        } else if (ev.type == "error") {
                            res->write("data: {\"error\": \"" + json_util::escape(ev.content) + "\"}\n\n");
                            res->end();
                        }
                    });
                }).detach();
            }
        });
    }).post("/api/chat", [this](auto *res, auto *req) {
        auto aborted = std::make_shared<bool>(false);
        auto body_buffer = std::make_shared<std::string>();

        res->onAborted([aborted]() {
            *aborted = true;
            spdlog::warn("HTTP request aborted by client");
        });

        res->onData([this, res, aborted, body_buffer](std::string_view data, bool last) {
            if (*aborted) return;
            body_buffer->append(data.data(), data.length());
            if (last) {
                simdjson::dom::parser parser;
                simdjson::dom::element x;
                auto error = parser.parse(*body_buffer).get(x);
                if (error) {
                    res->writeStatus("400 Bad Request")->end("Invalid JSON");
                    return;
                }

                std::string_view message_sv, session_id_sv, requested_model_sv;
                if (!x["message"].get(message_sv)) {}
                if (!x["session_id"].get(session_id_sv)) {}
                if (!x["model"].get(requested_model_sv)) {}
                
                std::string message = std::string(message_sv);
                std::string session_id = std::string(session_id_sv);
                std::string requested_model = std::string(requested_model_sv.empty() ? "default" : requested_model_sv);

                spdlog::info("Internal Chat API: model={}, session={}, msg_len={}", requested_model, session_id, message.length());

                res->writeStatus("200 OK")
                   ->writeHeader("Content-Type", "text/event-stream")
                   ->writeHeader("Cache-Control", "no-cache")
                   ->writeHeader("Connection", "keep-alive")
                   ->writeHeader("Access-Control-Allow-Origin", "*");

                boost::fibers::fiber([this, res, aborted, message, session_id]() {
                    agent_->run(message, session_id, [res, aborted](const AgentEvent& ev) {
                        if (*aborted) return;
                        std::string chunk = "data: {\"type\":\"" + json_util::escape(ev.type) + "\",\"content\":\"" + json_util::escape(ev.content) + "\"}\n\n";
                        res->write(chunk);
                        if (ev.type == "done" || ev.type == "error") {
                            res->end();
                        }
                    });
                }).detach();
            }
        });
    });

    uws_app->listen(9000, LIBUS_LISTEN_DEFAULT, [this](auto *listen_socket) {
        if (listen_socket) {
            spdlog::info("FiberNode listening on port 9000");
            this->listen_socket_ = listen_socket;
        } else {
            spdlog::error("FiberNode failed to listen on port 9000");
        }
    });

    // Keep-alive fiber
    boost::fibers::fiber([this]() {
        while (running_.load()) {
            boost::this_fiber::sleep_for(std::chrono::milliseconds(500));
        }
        shutdown_cv_.notify_all();
    }).detach();

    // Instead of tight yield loop, wait for shutdown signal
    {
        std::unique_lock<boost::fibers::mutex> lock(shutdown_mtx_);
        shutdown_cv_.wait(lock, [this] { return !running_.load(); });
    }
    
    spdlog::info("Fiber node thread exiting");
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
