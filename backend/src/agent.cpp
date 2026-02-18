#include "agent.hpp"
#include "agent/subagent.hpp"

#include <uv.h>
#include <fiber.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "tools/terminal.hpp"
#include "tools/file.hpp"
#include "tools/web.hpp"
#include "tools/spawn.hpp"

#include <queue>
#include <mutex>
#include <condition_variable>

static std::queue<std::function<void()>> g_spawn_queue;
static std::mutex g_spawn_mtx;
static uv_async_t g_spawn_async;

void init_spawn_system() {
    uv_async_init(uv_default_loop(), &g_spawn_async, [](uv_async_t*) {
        std::unique_lock<std::mutex> lock(g_spawn_mtx);
        while (!g_spawn_queue.empty()) {
            auto task = std::move(g_spawn_queue.front());
            g_spawn_queue.pop();
            lock.unlock();
            task();
            lock.lock();
        }
    });
}

void spawn_in_fiber(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(g_spawn_mtx);
        g_spawn_queue.push(std::move(task));
    }
    uv_async_send(&g_spawn_async);
}

Agent::~Agent() = default;

Agent::Agent() {
    api_key_   = std::getenv("OPENAI_API_KEY")  ? std::getenv("OPENAI_API_KEY")  : "";
    api_base_  = std::getenv("OPENAI_API_BASE") ? std::getenv("OPENAI_API_BASE") : "api.openai.com";
    model_     = std::getenv("OPENAI_MODEL")    ? std::getenv("OPENAI_MODEL")    : "gpt-4-turbo";
    workspace_ = std::getenv("WORKSPACE_DIR")   ? std::getenv("WORKSPACE_DIR")   : ".";

    // Build the LLM call function (captures this)
    LLMCallFn llm_fn = [this](
        const std::vector<Message>& messages,
        AgentEventCallback on_event
    ) -> std::string {
        return call_llm(messages, on_event);
    };

    loop_ = std::make_unique<AgentLoop>(workspace_, llm_fn, /*max_iterations=*/10);
    sessions_ = std::make_unique<SessionManager>(workspace_);
    subagents_ = std::make_unique<SubagentManager>(workspace_, llm_fn);

    // Register built-in tools
    loop_->register_tool("terminal",   std::make_shared<TerminalTool>());
    loop_->register_tool("read_file",  std::make_shared<ReadFileTool>());
    loop_->register_tool("write_file", std::make_shared<WriteFileTool>());
    loop_->register_tool("web_search", std::make_shared<WebSearchTool>());
    loop_->register_tool("web_fetch",  std::make_shared<WebFetchTool>());
    loop_->register_tool("spawn",      std::make_shared<SpawnTool>(*subagents_));

    spdlog::info("Agent initialized: model={} workspace={}", model_, workspace_);
}
void Agent::run(
    const std::string& user_message,
    const std::string& session_id,
    AgentEventCallback on_event
) {
    auto session = sessions_->get_or_create(session_id);
    
    // Update tools that need session context
    auto spawn_tool = std::dynamic_pointer_cast<SpawnTool>(loop_->get_tool("spawn"));
    if (spawn_tool) spawn_tool->set_context(session_id);

    struct RunData {
        Agent* self;
        std::string msg;
        Session* session;
        AgentEventCallback on_event;
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
    };

    auto* data = new RunData{this, user_message, &session, std::move(on_event)};

    spawn_in_fiber([data]() {
        fiber_create([](void* arg) -> void* {
            auto* d = (RunData*)arg;
            try {
                d->self->loop_->process(d->msg, *d->session, d->on_event);
            } catch (const std::exception& e) {
                d->on_event({"error", e.what()});
            }
            {
                std::lock_guard<std::mutex> lock(d->mtx);
                d->done = true;
            }
            d->cv.notify_one();
            return nullptr;
        }, data, NULL, 0);
    });

    // Wait for the fiber to complete in the Crow thread
    std::unique_lock<std::mutex> lock(data->mtx);
    data->cv.wait(lock, [&]{ return data->done; });
    delete data;

    sessions_->save(session);
}

// Global libuv loop for the agent
static uv_loop_t* g_loop = uv_default_loop();

std::string Agent::call_llm(
    const std::vector<Message>& messages,
    AgentEventCallback on_event
) {
    struct CallData {
        Agent* self;
        const std::vector<Message>* messages;
        AgentEventCallback on_event;
        std::string result;
        fiber_t fiber;
        uv_async_t async;
        bool completed = false;
    };

    auto* data = new CallData{this, &messages, on_event, "", fiber_ident()};
    
    uv_async_init(g_loop, &data->async, [](uv_async_t* handle) {
        auto* d = (CallData*)handle->data;
        fiber_resume(d->fiber);
    });
    data->async.data = data;

    std::thread t([data]() {
        // Build payload and messages
        json j_messages = json::array();
        for (const auto& m : *data->messages) {
            j_messages.push_back({{"role", m.role}, {"content", m.content}});
        }
        json payload = {
            {"model",    data->self->model_},
            {"messages", j_messages},
            {"stream",   true}
        };

        std::string api_url = data->self->api_base_;
        if (api_url.find("http://") != 0 && api_url.find("https://") != 0) {
            api_url = "https://" + api_url;
        }

        httplib::Client cli(api_url);
        cli.set_bearer_token_auth(data->self->api_key_);
        cli.set_read_timeout(300, 0);

        std::string full_response;
        httplib::Request req;
        req.method = "POST";
        req.path = "/v1/chat/completions";
        req.headers = {{"Content-Type", "application/json"}};
        req.body = payload.dump();
        req.content_receiver = [&](const char* chunk_data, size_t len, uint64_t /*off*/, uint64_t /*total*/) {
            spdlog::debug("LLM Chunk received: {} bytes", len);
            std::string chunk(chunk_data, len);
            std::istringstream ss(chunk);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.rfind("data: ", 0) != 0) continue;
                std::string json_str = line.substr(6);
                if (json_str == "[DONE]") continue;
                try {
                    auto j = json::parse(json_str);
                    if (j["choices"][0]["delta"].contains("content")) {
                        std::string tok = j["choices"][0]["delta"]["content"];
                        data->on_event({"token", tok});
                        full_response += tok;
                    }
                } catch (...) {}
            }
            return true;
        };

        auto res = cli.send(req);
        if (!res || res->status != 200) {
            int code = res ? res->status : 0;
            spdlog::error("LLM HTTP error: {}", code);
            data->result = "Error: LLM call failed (HTTP " + std::to_string(code) + ")";
        } else {
            data->result = full_response;
        }

        // Pulse the loop to resume the fiber
        uv_async_send(&data->async);
    });
    t.detach();

    // Suspend fiber until LLM call completes
    fiber_suspend(0);

    std::string res = data->result;
    
    // Cleanup libuv async handle (must be done on loop thread)
    // For simplicity here, we assume the next fiber tick will handle it 
    // or we could use another async to close it.
    uv_close((uv_handle_t*)&data->async, [](uv_handle_t* h) {
        delete (CallData*)h->data;
    });

    return res;
}
