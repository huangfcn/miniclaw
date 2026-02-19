#include "fiber_pool.hpp"
#include "curl_manager.hpp"
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
            task();
            lock.lock();
        }
    });
    async_.data = this;
}

FiberNode::~FiberNode() {
    stop();
    if (app_) delete app_;
    uv_loop_close(&loop_);
}

void FiberNode::start() {
    running_ = true;
    thread_ = std::thread(&FiberNode::thread_func, this);
}

void FiberNode::stop() {
    if (running_) {
        running_ = false;
        // In uWS, we'd normally close the listen socket, but for simplicity:
        uv_stop(&loop_);
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
    
    // Initialize Fiber for this thread
    FiberThreadStartup();

    // Initialize Thread-Local Curl Manager
    CurlMultiManager::instance().init(&loop_);

    // Initialize uWebSockets Loop wrapper for this thread using our native loop
    uWS::Loop::get(&loop_);

    // Initialize uWebSockets App
    app_ = new uWS::App();

    app_->get("/api/health", [](auto *res, auto *req) {
        res->end("OK");
    }).options("/*", [](auto *res, auto *req) {
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
           ->writeHeader("Access-Control-Allow-Headers", "content-type, authorization")
           ->end();
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Content-Type", "application/json");
        
        std::string response = "{\"object\":\"list\",\"data\":["
            "{\"id\":\"miniclaw\",\"object\":\"model\",\"created\":1686935002,\"owned_by\":\"miniclaw\"},"
            "{\"id\":\"gpt-4o-mini\",\"object\":\"model\",\"created\":1721251200,\"owned_by\":\"openai\"},"
            "{\"id\":\"gpt-5\",\"object\":\"model\",\"created\":1750000000,\"owned_by\":\"openai\"},"
            "{\"id\":\"gpt-5-mini\",\"object\":\"model\",\"created\":1750000001,\"owned_by\":\"openai\"}"
        "]}";
        res->end(response);
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

                // Extract last user message for miniclaw's current single-message processing
                std::string message = "";
                std::string_view requested_model_sv;
                (void)x["model"].get(requested_model_sv);
                std::string requested_model = std::string(requested_model_sv.empty() ? "unknown" : requested_model_sv);

                simdjson::dom::array messages;
                if (!x["messages"].get(messages)) {
                    if (messages.size() > 0) {
                        simdjson::dom::element last_msg;
                        if (!messages.at(messages.size() - 1).get(last_msg)) {
                            std::string_view content_sv;
                            (void)last_msg["content"].get(content_sv);
                            message = std::string(content_sv);
                        }
                    }
                }
                
                spdlog::info("OpenAI Chat Completion: model={}, session=default, msg_len={}", requested_model, message.length());
                
                // Open WebUI often doesn't send a session_id in the OpenAI request
                // We'll use a default or generate one if needed, but for now just "default"
                std::string session_id = "default";

                res->writeStatus("200 OK")
                   ->writeHeader("Content-Type", "text/event-stream")
                   ->writeHeader("Cache-Control", "no-cache")
                   ->writeHeader("Connection", "keep-alive")
                   ->writeHeader("Access-Control-Allow-Origin", "*");

                struct RequestContext {
                    FiberNode* node;
                    uWS::HttpResponse<false>* res;
                    std::string message;
                    std::string session_id;
                    std::string api_key;
                    std::shared_ptr<bool> aborted;
                    std::string chat_id;
                };

                auto ctx = new RequestContext{
                    this, res, std::move(message), std::move(session_id), std::move(auth_header), aborted,
                    "chatcmpl-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
                };

                fiber_create([](void* arg) -> void* {
                    std::unique_ptr<RequestContext> ctx((RequestContext*)arg);
                    
                    ctx->node->agent_->run(ctx->message, ctx->session_id, ctx->api_key, [res = ctx->res, aborted = ctx->aborted, chat_id = ctx->chat_id](const AgentEvent& ev) {
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
                        } else if (ev.type == "error") {
                            // Non-standard but helpful
                            res->write("data: {\"error\": \"" + json_util::escape(ev.content) + "\"}\n\n");
                            res->end();
                        }
                    });
                    return nullptr;
                }, ctx, NULL, 1024 * 1024);
            }
        });
    }).post("/api/chat", [this](auto *res, auto *req) {
        auto aborted = std::make_shared<bool>(false);
        auto body_buffer = std::make_shared<std::string>();

        res->onAborted([aborted]() {
            *aborted = true;
            spdlog::warn("HTTP request aborted by client");
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

                std::string_view message_sv, session_id_sv, requested_model_sv;
                (void)x["message"].get(message_sv);
                (void)x["session_id"].get(session_id_sv);
                (void)x["model"].get(requested_model_sv);

                std::string message = std::string(message_sv);
                std::string session_id = std::string(session_id_sv);
                std::string requested_model = std::string(requested_model_sv.empty() ? "default" : requested_model_sv);

                spdlog::info("Internal Chat API: model={}, session={}, msg_len={}", requested_model, session_id, message.length());

                // Send SSE headers now that we know it's a valid request
                res->writeStatus("200 OK")
                   ->writeHeader("Content-Type", "text/event-stream")
                   ->writeHeader("Cache-Control", "no-cache")
                   ->writeHeader("Connection", "keep-alive")
                   ->writeHeader("Access-Control-Allow-Origin", "*");

                struct RequestContext {
                    FiberNode* node;
                    uWS::HttpResponse<false>* res;
                    std::string message;
                    std::string session_id;
                    std::string api_key;
                    std::shared_ptr<bool> aborted;
                };

                auto ctx = new RequestContext{
                    this, res, std::move(message), std::move(session_id), std::move(auth_header), aborted
                };

                // Create fiber to handle the request
                fiber_create([](void* arg) -> void* {
                    std::unique_ptr<RequestContext> ctx((RequestContext*)arg);
                    
                    ctx->node->agent_->run(ctx->message, ctx->session_id, ctx->api_key, [res = ctx->res, aborted = ctx->aborted](const AgentEvent& ev) {
                        if (*aborted) return;
                        std::string chunk = "data: {\"type\":\"" + json_util::escape(ev.type) + "\",\"content\":\"" + json_util::escape(ev.content) + "\"}\n\n";
                        res->write(chunk);
                        if (ev.type == "done" || ev.type == "error") {
                            res->end();
                        }
                    });
                    return nullptr;
                }, ctx, NULL, 1024 * 1024);
            }
        });
    });

    // Listen with REUSE_PORT (default in many uWS versions or handled by OS)
    app_->listen(9000, LIBUS_LISTEN_DEFAULT, [this](auto *listen_socket) {
        if (listen_socket) {
            spdlog::info("FiberNode listening on port 9000");
        } else {
            spdlog::error("FiberNode failed to listen on port 9000");
        }
    });

    // Pulse Libuv from the Fiber Scheduler
    fibthread_args_t fiber_args;
    fiber_args.fiberSchedulerCallback = [](void* arg) -> bool {
        uv_run((uv_loop_t*)arg, UV_RUN_NOWAIT);
        return true;
    };
    fiber_args.args = &loop_;

    fiber_thread_entry(&fiber_args);
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
