#include <uv.h>
#include <fiber.hpp>
#include "agent.hpp"
#include <spdlog/spdlog.h>
#include <simdjson.h>
#include "json_util.hpp"
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

void init_spawn_system() {
    curl_global_init(CURL_GLOBAL_ALL);
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

    // Build the LLM call function (now takes tools_json too)
    LLMCallFn llm_fn = [this](
        const std::vector<Message>& messages,
        const std::string& tools_json,
        AgentEventCallback on_event
    ) -> LLMResponse {
        return call_llm(messages, tools_json, on_event);
    };

    loop_ = std::make_unique<AgentLoop>(workspace_, llm_fn, /*max_iterations=*/10);
    sessions_ = std::make_unique<SessionManager>(workspace_);
    subagents_ = std::make_unique<SubagentManager>(workspace_, llm_fn);

    // Register built-in tools
    loop_->register_tool("exec",       std::make_shared<TerminalTool>());
    loop_->register_tool("read_file",  std::make_shared<ReadFileTool>());
    loop_->register_tool("write_file", std::make_shared<WriteFileTool>());
    loop_->register_tool("edit_file",  std::make_shared<EditFileTool>());
    loop_->register_tool("list_dir",   std::make_shared<ListDirTool>());
    loop_->register_tool("web_search", std::make_shared<WebSearchTool>());
    loop_->register_tool("web_fetch",  std::make_shared<WebFetchTool>());
    loop_->register_tool("spawn",      std::make_shared<SpawnTool>(*subagents_));

    spdlog::info("Agent initialized: model={} workspace={}", model_, workspace_);
}

void Agent::run(
    const std::string& user_message,
    const std::string& session_id,
    const std::string& api_key,
    AgentEventCallback on_event,
    const std::string& channel
) {
    auto session = sessions_->get_or_create(session_id);

    auto* fiber_tcb = fiber_ident();
    if (fiber_tcb) {
        fiber_set_localdata(fiber_tcb, 0, reinterpret_cast<uint64_t>(new std::string(session_id)));
        fiber_set_localdata(fiber_tcb, 1, reinterpret_cast<uint64_t>(new std::string(api_key.empty() ? api_key_ : api_key)));
    }

    try {
        loop_->process(user_message, session, on_event, channel, session_id);
    } catch (const std::exception& e) {
        spdlog::error("Error in Agent::run: {}", e.what());
        on_event({"error", e.what()});
    }

    if (fiber_tcb) {
        auto* s0 = reinterpret_cast<std::string*>(fiber_get_localdata(fiber_tcb, 0));
        auto* s1 = reinterpret_cast<std::string*>(fiber_get_localdata(fiber_tcb, 1));
        delete s0;
        delete s1;
        fiber_set_localdata(fiber_tcb, 0, 0);
        fiber_set_localdata(fiber_tcb, 1, 0);
    }

    sessions_->save(session);
}

// ─── Serialize a Message vector to JSON ──────────────────────────────────────

static std::string serialize_messages(const std::vector<Message>& messages) {
    std::string j = "[";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) j += ",";
        const auto& m = messages[i];

        j += "{\"role\":\"" + json_util::escape(m.role) + "\"";

        if (m.role == "assistant" && !m.tool_calls_json.empty()) {
            // Assistant message with tool_calls (may also have text content)
            if (!m.content.empty()) {
                j += ",\"content\":\"" + json_util::escape(m.content) + "\"";
            } else {
                j += ",\"content\":null";
            }
            j += ",\"tool_calls\":" + m.tool_calls_json;
        } else if (m.role == "tool") {
            // Tool result message
            j += ",\"tool_call_id\":\"" + json_util::escape(m.tool_call_id) + "\"";
            j += ",\"name\":\"" + json_util::escape(m.name) + "\"";
            j += ",\"content\":\"" + json_util::escape(m.content) + "\"";
        } else {
            j += ",\"content\":\"" + json_util::escape(m.content) + "\"";
        }

        j += "}";
    }
    j += "]";
    return j;
}

// ─── Accumulator for streaming tool_calls ────────────────────────────────────

struct ToolCallAccum {
    std::string id;
    std::string name;
    std::string arguments; // accumulated incrementally
};

// ─── call_llm: sends messages + tools, parses streaming tool_calls ───────────

LLMResponse Agent::call_llm(
    const std::vector<Message>& messages,
    const std::string& tools_json,
    AgentEventCallback on_event
) {
    // Per-call state shared between write callback and cleanup
    struct CallData {
        Agent* self;
        AgentEventCallback on_event;
        std::string text_content;      // accumulates delta.content tokens
        std::string buffer;            // raw SSE buffer
        fiber_t fiber;
        std::function<void(CURLcode)> completion_cb;
        struct curl_slist* headers = nullptr;
        simdjson::dom::parser parser;

        // tool_calls accumulation
        std::vector<ToolCallAccum> tool_calls;
    };

    auto* data = new CallData();
    data->self = this;
    data->on_event = on_event;
    data->fiber = fiber_ident();
    CURL* easy = curl_easy_init();

    data->completion_cb = [data, easy](CURLcode code) {
        if (code != CURLE_OK) {
            spdlog::error("CURL error: {}", curl_easy_strerror(code));
        }
        long http_code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            spdlog::error("LLM HTTP Error: {}", http_code);
            if (data->text_content.empty()) {
                data->text_content = "Error: LLM API returned HTTP " + std::to_string(http_code);
            }
        }
        fiber_resume(data->fiber);
    };

    // ── Build payload ──────────────────────────────────────────────────────
    std::string j_messages = serialize_messages(messages);

    std::string payload_str = "{\"model\":\"" + model_ + "\","
        "\"messages\":" + j_messages + ","
        "\"stream\":true";

    // Add tools array if non-empty (at least "[]")
    if (!tools_json.empty() && tools_json != "[]") {
        payload_str += ",\"tools\":" + tools_json;
        payload_str += ",\"tool_choice\":\"auto\"";
    }
    payload_str += "}";

    spdlog::debug("LLM payload (first 1000 chars): {}", payload_str.substr(0, 1000));
    if (payload_str.size() > 1000) spdlog::debug("LLM payload (last 500 chars): {}", payload_str.substr(payload_str.size() - 500));

    // ── Headers ────────────────────────────────────────────────────────────
    data->headers = curl_slist_append(data->headers, "Content-Type: application/json");

    std::string effective_key = api_key_;
    auto* fiber_tcb = fiber_ident();
    if (fiber_tcb) {
        auto* key_ptr = reinterpret_cast<std::string*>(fiber_get_localdata(fiber_tcb, 1));
        if (key_ptr && !key_ptr->empty()) effective_key = *key_ptr;
    }
    std::string auth = "Authorization: Bearer " + effective_key;
    data->headers = curl_slist_append(data->headers, auth.c_str());

    // ── URL ────────────────────────────────────────────────────────────────
    std::string url = api_base_;
    if (url.find("http://") != 0 && url.find("https://") != 0) url = "https://" + url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() < 3 || url.substr(url.size() - 3) != "/v1") url += "/v1";
    url += "/chat/completions";
    spdlog::info("LLM URL: {}", url);

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, payload_str.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, data->headers);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, &data->completion_cb);

    // ── Write callback: parse SSE stream ───────────────────────────────────
    auto write_cb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        size_t total = size * nmemb;
        auto* d = (CallData*)userdata;
        d->buffer.append(ptr, total);

        size_t pos;
        while ((pos = d->buffer.find('\n')) != std::string::npos) {
            std::string line = d->buffer.substr(0, pos);
            d->buffer.erase(0, pos + 1);

            // Strip carriage return
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.rfind("data: ", 0) != 0) {
                // Non-data line: might be an error JSON
                if (!line.empty()) {
                    spdlog::debug("LLM raw line: {}", line);
                    simdjson::dom::element j;
                    auto padded = simdjson::padded_string(line);
                    if (!d->parser.parse(padded).get(j)) {
                        simdjson::dom::element err;
                        if (!j["error"].get(err)) {
                            std::string_view msg;
                            if (!err["message"].get(msg))
                                d->text_content = "Error: " + std::string(msg);
                            else
                                d->text_content = "Error: " + simdjson::to_string(err);
                        }
                    }
                }
                continue;
            }

            std::string json_str = line.substr(6);
            if (json_str == "[DONE]" || json_str.empty()) continue;

            simdjson::dom::element j;
            auto padded = simdjson::padded_string(json_str);
            auto error = d->parser.parse(padded).get(j);
            if (error) {
                spdlog::warn("simdjson parse error in write_cb: {}", json_str.substr(0, 200));
                continue;
            }

            simdjson::dom::array choices;
            if (j["choices"].get(choices) || choices.size() == 0) continue;

            simdjson::dom::element delta;
            if (choices.at(0)["delta"].get(delta)) continue;

            // ── Text content tokens ────────────────────────────────────────
            std::string_view content_sv;
            if (!delta["content"].get(content_sv) && !content_sv.empty()) {
                std::string tok(content_sv);
                d->on_event({"token", tok});
                d->text_content += tok;
            }

            // ── Tool call chunks ───────────────────────────────────────────
            simdjson::dom::array tc_arr;
            if (!delta["tool_calls"].get(tc_arr)) {
                for (auto tc_elem : tc_arr) {
                    int64_t idx = 0;
                    tc_elem["index"].get(idx);

                    // Grow accumulator array if needed
                    while ((int64_t)d->tool_calls.size() <= idx) {
                        d->tool_calls.push_back({});
                    }
                    auto& accum = d->tool_calls[idx];

                    // id
                    std::string_view id_sv;
                    if (!tc_elem["id"].get(id_sv)) accum.id = std::string(id_sv);

                    // function.name
                    simdjson::dom::element fn_elem;
                    if (!tc_elem["function"].get(fn_elem)) {
                        std::string_view name_sv;
                        if (!fn_elem["name"].get(name_sv) && !name_sv.empty())
                            accum.name = std::string(name_sv);

                        // function.arguments (streamed in chunks)
                        std::string_view args_sv;
                        if (!fn_elem["arguments"].get(args_sv))
                            accum.arguments += std::string(args_sv);
                    }
                }
            }
        }
        return total;
    };

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, (curl_write_callback)write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 300L);

    spdlog::debug("Starting Async LLM call via CurlMulti for fiber {}", (void*)data->fiber);
    CurlMultiManager::instance().add_handle(easy);
    fiber_suspend(0);
    spdlog::debug("Async LLM call resumed for fiber {}", (void*)data->fiber);

    // Process leftover buffer
    if (!data->buffer.empty()) {
        std::string line = data->buffer;
        if (line.rfind("data: ", 0) != 0) {
            simdjson::dom::element j;
            auto padded = simdjson::padded_string(line);
            if (!data->parser.parse(padded).get(j)) {
                simdjson::dom::element err;
                if (!j["error"].get(err)) {
                    std::string_view msg;
                    if (!err["message"].get(msg))
                        data->text_content = "Error: " + std::string(msg);
                }
            }
        }
    }

    // Cleanup
    CurlMultiManager::instance().remove_handle(easy);
    curl_slist_free_all(data->headers);
    curl_easy_cleanup(easy);

    // Assemble result
    LLMResponse result;
    result.content = data->text_content;

    for (const auto& accum : data->tool_calls) {
        ToolCall tc;
        tc.id = accum.id.empty() ? ("call_" + std::to_string(rand())) : accum.id;
        tc.name = accum.name;
        tc.arguments_json = accum.arguments.empty() ? "{}" : accum.arguments;
        result.tool_calls.push_back(tc);
        spdlog::debug("Accumulated tool_call: name={} args={}", tc.name, tc.arguments_json.substr(0, 200));
    }

    delete data;
    return result;
}
