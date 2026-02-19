#include <uv.h>
#include <fiber.hpp>
#include "agent.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <simdjson.h>
#include <curl/curl.h>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>

#include "agent/subagent.hpp"
#include "agent/fiber_pool.hpp"
#include "agent/curl_manager.hpp"
#include "tools/terminal.hpp"
#include "tools/file.hpp"
#include "tools/web.hpp"
#include "tools/spawn.hpp"
#include "config.hpp"

// Remove old global spawn system
// static std::queue<std::function<void()>> g_spawn_queue;
// static std::mutex g_spawn_mtx;
// static uv_async_t g_spawn_async;

void init_spawn_system() {
    curl_global_init(CURL_GLOBAL_ALL);
    // CurlMultiManager is now initialized per FiberNode thread
    spdlog::info("Spawn system (FiberPool) ready");
}

void spawn_in_fiber(std::function<void()> task) {
    FiberPool::instance().spawn(std::move(task));
}

Agent::~Agent() = default;

Agent::Agent() {
    api_key_   = Config::instance().openai_api_key();
    api_base_  = std::getenv("OPENAI_API_BASE") ? std::getenv("OPENAI_API_BASE") : "api.openai.com";
    model_     = Config::instance().openai_model();
    workspace_ = Config::instance().memory_workspace();

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
    const std::string& api_key,
    AgentEventCallback on_event
) {
    auto session = sessions_->get_or_create(session_id);
    
    // Set session_id and api_key in Fiber Local Storage
    // Index 0: session_id, Index 1: api_key
    auto* fiber_tcb = fiber_ident();
    if (fiber_tcb) {
        fiber_set_localdata(fiber_tcb, 0, reinterpret_cast<uint64_t>(new std::string(session_id)));
        fiber_set_localdata(fiber_tcb, 1, reinterpret_cast<uint64_t>(new std::string(api_key.empty() ? api_key_ : api_key)));
    }

    try {
        loop_->process(user_message, session, on_event);
    } catch (const std::exception& e) {
        spdlog::error("Error in Agent::run: {}", e.what());
        on_event({"error", e.what()});
    }

    // Cleanup Fiber Local Storage
    if (fiber_tcb) {
        auto* s0 = reinterpret_cast<std::string*>(fiber_get_localdata(fiber_tcb, 0));
        auto* s1 = reinterpret_cast<std::string*>(fiber_get_localdata(fiber_tcb, 1));
        delete s0;
        delete s1;
        fiber_set_localdata(fiber_tcb, 0, 0);
        fiber_set_localdata(fiber_tcb, 1, 0);
    }

    // Save updated session state
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
        simdjson::dom::parser parser;
    };

    auto* data = new CallData();
    data->self = this;
    data->on_event = on_event;
    data->fiber = fiber_ident();
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
    // spdlog::info("---- send to LLM -----\n{}", payload_str); // Log LLM Input

    // Headers
    data->headers = curl_slist_append(data->headers, "Content-Type: application/json");
    
    // Get API key from Fiber Local Storage (Index 1) or fallback to internal
    std::string effective_key = api_key_;
    auto* fiber_tcb = fiber_ident();
    if (fiber_tcb) {
        auto* key_ptr = reinterpret_cast<std::string*>(fiber_get_localdata(fiber_tcb, 1));
        if (key_ptr && !key_ptr->empty()) effective_key = *key_ptr;
    }

    std::string auth = "Authorization: Bearer " + effective_key;
    data->headers = curl_slist_append(data->headers, auth.c_str());

    // URL
    std::string url = api_base_;
    if (url.find("http://") != 0 && url.find("https://") != 0) {
        url = "https://" + url;
    }
    // Remove trailing slash if present
    while (!url.empty() && url.back() == '/') url.pop_back();
    // Append /v1 only if not already present
    if (url.size() < 3 || url.substr(url.size() - 3) != "/v1") {
        url += "/v1";
    }
    url += "/chat/completions";
    spdlog::info("LLM URL: {}", url);

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
                simdjson::dom::element j;
                auto error = d->parser.parse(json_str).get(j);
                if (!error) {
                    simdjson::dom::array choices;
                    if (!j["choices"].get(choices) && choices.size() > 0) {
                        simdjson::dom::element delta;
                        if (!choices.at(0)["delta"].get(delta)) {
                            std::string_view content_sv;
                            if (!delta["content"].get(content_sv)) {
                                std::string tok = std::string(content_sv);
                                d->on_event({"token", tok});
                                d->result += tok;
                            }
                        }
                    }
                } else {
                    spdlog::warn("simdjson parse error in curl write_cb: {} (str: {})", (int)error, json_str);
                }
            } else if (!line.empty()) {
                spdlog::debug("Unexpected LLM response line: {}", line);
                if (d->result.empty() && line.find('{') != std::string::npos) {
                    // Likely an error JSON
                    simdjson::dom::element j;
                    if (!d->parser.parse(line).get(j)) {
                        simdjson::dom::element err;
                        if (!j["error"].get(err)) {
                            // simdjson doesn't have a direct dump() like nlohmann for elements
                            // We can use simdjson::to_string(err) in recent versions or just get specific fields
                            std::string_view msg;
                            if (!err["message"].get(msg)) {
                                d->result = "Error: " + std::string(msg);
                            }
                        }
                    }
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

    // Process leftover buffer (e.g. error JSON without newline)
    if (!data->buffer.empty()) {
        std::string line = data->buffer;
        if (line.rfind("data: ", 0) == 0) {
            // ... (optional: handle data: [DONE] if missing newline)
        } else {
            simdjson::dom::element j;
            if (!data->parser.parse(line).get(j)) {
                simdjson::dom::element err;
                if (!j["error"].get(err)) {
                    std::string_view msg;
                    if (!err["message"].get(msg)) {
                        data->result = "Error: " + std::string(msg);
                    }
                }
            }
        }
    }

    // Cleanup
    CurlMultiManager::instance().remove_handle(easy);
    curl_slist_free_all(data->headers);
    curl_easy_cleanup(easy);

    std::string final_res = data->result;
    // spdlog::info("----- receive from LLM -----\n{}", final_res); // Log LLM Output
    delete data;
    return final_res;
}
