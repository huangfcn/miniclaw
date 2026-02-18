#include <uv.h>
#include <fiber.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>

#include "tools/terminal.hpp"
#include "tools/file.hpp"
#include "tools/web.hpp"
#include "tools/spawn.hpp"

#include <queue>
#include <mutex>
#include <condition_variable>
#include "agent/curl_manager.hpp"

static std::queue<std::function<void()>> g_spawn_queue;
static std::mutex g_spawn_mtx;
static uv_async_t g_spawn_async;

void init_spawn_system() {
    auto* loop = uv_default_loop();
    curl_global_init(CURL_GLOBAL_ALL);
    CurlMultiManager::instance().init(loop);
    spdlog::info("Spawn system initialized with Async Curl manager");

    uv_async_init(loop, &g_spawn_async, [](uv_async_t*) {
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
        }, data, NULL, 512 * 1024);
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
        AgentEventCallback on_event;
        std::string result;
        std::string buffer; // raw buffer for parsing chunks
        fiber_t fiber;
        std::function<void(CURLcode)> completion_cb;
        struct curl_slist* headers = nullptr;
    };

    auto* data = new CallData{this, on_event, "", "", fiber_ident(), nullptr, nullptr};
    data->completion_cb = [data](CURLcode code) {
        if (code != CURLE_OK) {
            spdlog::error("CURL error: {}", curl_easy_strerror(code));
            if (data->result.empty()) {
                data->result = "Error: LLM call failed (Curl " + std::to_string(code) + ")";
            }
        }
        fiber_resume(data->fiber);
    };

    CURL* easy = curl_easy_init();
    
    // Build payload
    json j_messages = json::array();
    for (const auto& m : messages) {
        j_messages.push_back({{"role", m.role}, {"content", m.content}});
    }
    json payload = {
        {"model",    model_},
        {"messages", j_messages},
        {"stream",   true}
    };
    std::string payload_str = payload.dump();

    // Headers
    data->headers = curl_slist_append(data->headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + api_key_;
    data->headers = curl_slist_append(data->headers, auth.c_str());

    // URL
    std::string url = api_base_;
    if (url.find("http://") != 0 && url.find("https://") != 0) {
        url = "https://" + url;
    }
    url += "/v1/chat/completions";

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, payload_str.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, data->headers);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, &data->completion_cb);
    
    // Write callback for streaming
    auto write_cb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        size_t total = size * nmemb;
        auto* d = (CallData*)userdata;
        d->buffer.append(ptr, total);

        size_t pos;
        while ((pos = d->buffer.find('\n')) != std::string::npos) {
            std::string line = d->buffer.substr(0, pos);
            d->buffer.erase(0, pos + 1);

            if (line.rfind("data: ", 0) == 0) {
                std::string json_str = line.substr(6);
                if (json_str == "[DONE]" || json_str.empty()) continue;
                try {
                    auto j = json::parse(json_str);
                    if (j.contains("choices") && !j["choices"].empty()) {
                        auto& delta = j["choices"][0]["delta"];
                        if (delta.contains("content")) {
                            std::string tok = delta["content"];
                            d->on_event({"token", tok});
                            d->result += tok;
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("JSON parse error in curl write_cb: {} (str: {})", e.what(), json_str);
                }
            }
        }
        return total;
    };
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, (curl_write_callback)write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, data);
    
    // SSL & Timeouts
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 300L);

    spdlog::debug("Starting Async LLM call via CurlMulti for fiber {}", (void*)data->fiber);
    // Start request
    CurlMultiManager::instance().add_handle(easy);

    // Wait for completion
    fiber_suspend(0);
    spdlog::debug("Async LLM call resumed for fiber {}", (void*)data->fiber);

    // Cleanup
    CurlMultiManager::instance().remove_handle(easy);
    curl_slist_free_all(data->headers);
    curl_easy_cleanup(easy);

    std::string final_res = data->result;
    delete data;
    return final_res;
}
