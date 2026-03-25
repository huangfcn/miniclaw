#include "App.h"
#include "fiber_pool.hpp"
#include "curl_manager.hpp"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <boost/fiber/algo/round_robin.hpp>
#else
#include <fiber.h>
#endif

#include <simdjson.h>
#include "agent.hpp"
#include "json_util.hpp"

extern "C" {
struct us_listen_socket_t;
void us_listen_socket_close(int ssl, struct us_listen_socket_t *ls);
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
        if (!self->running_.load() && self->listen_socket_) {
            us_listen_socket_close(0, self->listen_socket_);
            self->listen_socket_ = nullptr;
        }

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

static void* fiber_entry_proxy(void* arg) {
    auto* wrapper = static_cast<fiber_task_wrapper_t*>(arg);
    wrapper->task();
    delete wrapper;
    return nullptr;
}

void FiberNode::spawn(std::function<void()> task) {
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    uv_async_send(&async_);
#else
    // Third-party fiber mode
    auto* wrapper = new fiber_task_wrapper_t{std::move(task)};
    fiber_t fiber = fiber_create(fiber_entry_proxy, wrapper, nullptr, 0);
    fiber_resume(fiber);
#endif
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
        auto aborted = std::make_shared<std::atomic<bool>>(false);
        auto body_buffer = std::make_shared<std::string>();

        res->onAborted([aborted]() {
            *aborted = true;
            spdlog::warn("OpenAI API request aborted");
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

                spawn([this, res, aborted, chat_id, message, session_id]() {
                    agent_->run(message, session_id, [this, res, aborted, chat_id](const AgentEvent& ev) {
                        if (*aborted) return;

                        auto write_chunk = [res, aborted, chat_id, ev]() {
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
                        };

#ifdef _WIN32
                        write_chunk();
#else
                        this->spawn_back_on_loop(write_chunk);
#endif
                    });
                });
            }
        });
    }).post("/api/chat", [this](auto *res, auto *req) {
        auto aborted = std::make_shared<std::atomic<bool>>(false);
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

                spawn([this, res, aborted, message, session_id]() {
                    agent_->run(message, session_id, [this, res, aborted](const AgentEvent& ev) {
                        if (*aborted) return;
                        
                        auto write_chunk = [res, aborted, ev]() {
                            if (*aborted) return;
                            std::string chunk = "data: {\"type\":\"" + json_util::escape(ev.type) + "\",\"content\":\"" + json_util::escape(ev.content) + "\"}\n\n";
                            res->write(chunk);
                            if (ev.type == "done" || ev.type == "error") {
                                res->end();
                            }
                        };

#ifdef _WIN32
                        write_chunk();
#else
                        this->spawn_back_on_loop(write_chunk);
#endif
                    });
                });
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
