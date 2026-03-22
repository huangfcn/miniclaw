#pragma once
// AgentLoop — native OpenAI function-calling implementation
// Uses tool_calls from API response instead of XML text parsing.

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <sstream>
#include <chrono>
#include <ctime>
#include <spdlog/spdlog.h>

#include "context.hpp"
#include "session.hpp"
#include "../tools/tool.hpp"
#include "../config.hpp"
#include <simdjson.h>


// One tool call from the LLM
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;  // raw JSON string from OpenAI
};

// Structured LLM response (replaces plain std::string)
struct LLMResponse {
    std::string content;           // text content (may be empty if tool_calls present)
    std::vector<ToolCall> tool_calls;
    bool has_tool_calls() const { return !tool_calls.empty(); }
};

// Event types streamed back to the caller
struct AgentEvent {
    std::string type;    // "token" | "tool_start" | "tool_end" | "done" | "error"
    std::string content;
};

using EventCallback = std::function<void(const AgentEvent&)>;

// LLMCallFn is "synchronous" from the fiber's perspective.
using LLMCallFn = std::function<LLMResponse(
    const std::vector<Message>& messages,
    const std::string& tools_json,
    EventCallback on_event,
    const std::string& model,
    const std::string& endpoint,
    const std::string& provider
)>;

using EmbeddingFn = std::function<std::vector<float>(const std::string& text)>;

class AgentLoop {
public:
    enum class DistillationEvent {
        PERIODIC,
        COMPACTION,
        SESSION_END
    };

    AgentLoop(
        const std::string& workspace,
        LLMCallFn llm_fn,
        EmbeddingFn embed_fn,
        int max_iterations = 10
    )
        : context_(workspace, embed_fn)
        , llm_fn_(std::move(llm_fn))
        , embed_fn_(std::move(embed_fn))
        , max_iterations_(max_iterations)
    {}

    void register_tool(const std::string& name, std::shared_ptr<Tool> tool) {
        tools_[name] = std::move(tool);
    }

    std::shared_ptr<Tool> get_tool(const std::string& name) {
        if (tools_.count(name)) return tools_[name];
        return nullptr;
    }

    // Build the tools JSON array for the API request
    std::string build_tools_json() const {
        std::string result = "[";
        bool first = true;
        std::string names;
        for (const auto& [name, tool] : tools_) {
            if (!first) {
                result += ",";
                names += ", ";
            }
            result += tool->schema();
            names += name;
            first = false;
        }
        result += "]";
        spdlog::debug("Sending tools to LLM: [{}]", names);
        return result;
    }

    // Parse JSON arguments from a tool call into a string->string map
    // Handles both simple string values and nested objects (converted to string)
    static std::map<std::string, std::string> parse_arguments(const std::string& json_str) {
        std::map<std::string, std::string> result;
        if (json_str.empty() || json_str == "{}") return result;

        try {
            simdjson::dom::parser parser;
            simdjson::dom::element obj;
            // simdjson requires padded input
            auto padded = simdjson::padded_string(json_str);
            auto error = parser.parse(padded).get(obj);
            if (error) {
                spdlog::warn("parse_arguments: JSON parse failed for: {}", json_str);
                return result;
            }
            simdjson::dom::object o;
            if (!obj.get(o)) {
                for (auto [key, val] : o) {
                    std::string k(key);
                    std::string_view sv;
                    if (!val.get(sv)) {
                        result[k] = std::string(sv);
                    } else {
                        // Non-string value: convert to string
                        result[k] = simdjson::to_string(val);
                    }
                }
            }
        } catch (...) {
            spdlog::warn("parse_arguments: exception parsing: {}", json_str);
        }
        return result;
    }

    // Build the "assistant" message with tool_calls for the API
    static Message make_assistant_tool_call_message(const std::string& content, const std::vector<ToolCall>& calls) {
        Message msg;
        msg.role = "assistant";
        msg.content = content;
        // Build tool_calls JSON
        std::string tc_json = "[";
        for (size_t i = 0; i < calls.size(); ++i) {
            if (i > 0) tc_json += ",";
            tc_json += "{\"id\":\"" + json_escape(calls[i].id) + "\","
                       "\"type\":\"function\","
                       "\"function\":{\"name\":\"" + json_escape(calls[i].name) + "\","
                       "\"arguments\":\"" + json_escape(calls[i].arguments_json) + "\"}}";
        }
        tc_json += "]";
        msg.tool_calls_json = tc_json;
        return msg;
    }

    // Build a "tool" role message (result of a tool call)
    static Message make_tool_result_message(const std::string& tool_call_id, const std::string& tool_name, const std::string& result) {
        Message msg;
        msg.role = "tool";
        msg.tool_call_id = tool_call_id;
        msg.name = tool_name;
        msg.content = result;
        return msg;
    }

    void process(
        const std::string& user_message,
        Session& session,
        EventCallback on_event,
        const std::string& channel = "",
        const std::string& chat_id = ""
    ) {
        // 1. Add and Index user message (Layer 1)
        session.add_message("user", user_message);
        context_.memory().index_session_message(session.key, "user", user_message);

        int memory_window = 20;
        std::vector<Message> history = session.messages;
        if (history.size() > memory_window) {
            history.erase(history.begin(), history.end() - memory_window);
        }

        // build_messages handles the latest user_message correctly
        std::vector<Message> messages = context_.build_messages(history, user_message, channel, chat_id);

        int iteration = 0;
        bool finished_normally = false;

        while (iteration < max_iterations_) {
            ++iteration;
            std::string tools_json = build_tools_json();
            std::string model = Config::instance().conversation_model();
            std::string endpoint = Config::instance().conversation_endpoint();
            std::string provider = Config::instance().conversation_provider();

            LLMResponse response = llm_fn_(messages, tools_json, on_event, model, endpoint, provider);
            spdlog::debug("AgentLoop iteration {}: has_tool_calls={} content_len={}",
                iteration, response.has_tool_calls(), response.content.size());

            if (response.content.find("Error") == 0 && !response.has_tool_calls()) {
                on_event({"error", response.content});
                break;
            }

            if (response.has_tool_calls()) {
                // Progressive Logging: Append the assistant message with tool_calls to session
                Message asst_msg = make_assistant_tool_call_message(response.content, response.tool_calls);
                session.add_message(asst_msg);
                messages.push_back(asst_msg);

                // Execute each tool call
                for (const auto& tc : response.tool_calls) {
                    spdlog::info("Tool call: {}({})", tc.name, tc.arguments_json.substr(0, 200));
                    on_event({"tool_start", tc.name + ": " + tc.arguments_json});

                    std::string output;
                    if (tools_.count(tc.name)) {
                        auto args = parse_arguments(tc.arguments_json);
                        output = tools_.at(tc.name)->execute(args);
                    } else if (tc.name == "memory_search") {
                        auto args = parse_arguments(tc.arguments_json);
                        std::string query = args["query"];
                        auto results = context_.memory().search(query);
                        
                        std::stringstream ss;
                        ss << "Search Results for \"" << query << "\":\n";
                        if (results.empty()) ss << "(No results found)\n";
                        for (const auto& r : results) {
                            ss << "- [" << r.source << "] " << r.path << ": " << r.text.substr(0, 200) << " (Score: " << r.score << ")\n";
                        }
                        output = ss.str();
                    } else if (tc.name == "index_document") {
                        auto args = parse_arguments(tc.arguments_json);
                        std::string path = args["path"];
                        std::string text = args["text"];
                        std::vector<float> emb;
                        if (embed_fn_) emb = embed_fn_(text);
                        context_.memory().index().add_document(path, path, 0, 0, text, emb, "manual");
                        output = "Document indexed: " + path;
                    } else {
                        output = "Error: unknown tool '" + tc.name + "'";
                    }

                    on_event({"tool_end", output});
                    
                    // Progressive Logging: Add tool result to session
                    Message tool_msg = make_tool_result_message(tc.id, tc.name, output);
                    session.add_message(tool_msg);
                    messages.push_back(tool_msg);
                }
            } else {
                // Final answer - Index it
                if (!response.content.empty()) {
                    context_.memory().index_session_message(session.key, "assistant", response.content);
                }
                session.add_message("assistant", response.content);
                finished_normally = true;
                break;
            }
        }

        if (!finished_normally && iteration >= max_iterations_) {
            on_event({"error", "Max iterations reached"});
        }

        // 2. DISTILLATION CHECK (Always runs after loop)
        int start_idx = session.last_consolidated;
        int end_idx = (int)session.messages.size();

        std::string today = current_date();
        std::string yesterday = yesterday_date();
        std::string now_time = current_time_hhmm();
        std::string target_time = Config::instance().memory_distillation_time();

        bool l2_missed_days = (!session.last_consolidation_date.empty() && session.last_consolidation_date < yesterday);
        bool l2_today_due = (session.last_consolidation_date != today && now_time >= target_time);

        // L1 -> L2 (Daily log) triggers
        size_t estimated_tokens = session.estimate_tokens();
        size_t token_limit = Config::instance().memory_context_window();
        float floor_threshold = Config::instance().memory_compaction_threshold();
        bool context_near_full = (estimated_tokens > token_limit * floor_threshold);

        bool l1_threshold_hit = false;
        std::string trigger_strategy = Config::instance().memory_l1_distillation_trigger();
        if (trigger_strategy == "token_count") {
            int token_trigger_threshold = Config::instance().memory_l1_token_threshold();
            l1_threshold_hit = (estimated_tokens - session.last_distilled_token_count > (size_t)token_trigger_threshold);
        } else { // "message_count"
            l1_threshold_hit = (end_idx - start_idx > Config::instance().memory_l1_to_l2_threshold());
        }

        // Run Consolidation (L1 -> L3) if due
        if (l2_missed_days || l2_today_due) {
            if (consolidate_memory(session, start_idx, end_idx)) {
                session.last_consolidation_date = today;
            }
        }

        // Run Distillation (L1 -> L2) if due or context near full
        if (l1_threshold_hit || context_near_full || l2_missed_days || l2_today_due) {
            if (end_idx > start_idx) {
                DistillationEvent event = context_near_full ? DistillationEvent::COMPACTION : DistillationEvent::PERIODIC;
                distill_l1_to_l2(session, start_idx, end_idx, event);
            }
        }

        session.last_consolidated = end_idx;

        on_event({"done", ""});
    }

    void distill_l1_to_l2(Session& session, int start, int end, DistillationEvent event = DistillationEvent::PERIODIC) {
        spdlog::info("Distilling L1 to L2 for session: {} (Event: {}, Range: {}-{})", session.key, (int)event, start, end);
        
        if (end <= start) return;
        
        std::stringstream conv;
        for (int i = start; i < end; ++i) {
            conv << "[" << session.messages[i].role << "]: " << session.messages[i].content << "\n";
        }

        std::string event_name = "prompt_periodic";
        std::string default_tpl = "Summarize the following conversation segment into a durable daily log entry.";
        
        if (event == DistillationEvent::COMPACTION) {
            event_name = "prompt_compaction";
            default_tpl = "The session is near compaction. Store durable memories now.";
        } else if (event == DistillationEvent::SESSION_END) {
            event_name = "prompt_session";
            default_tpl = "This session is ending. Summarize the conversation.";
        }

        std::string template_str = Config::instance().load_prompt(event_name, default_tpl);
        
        // Simple replacement of {{CONVERSATION}}
        size_t pos = template_str.find("{{CONVERSATION}}");
        if (pos != std::string::npos) {
            template_str.replace(pos, 16, conv.str());
        } else {
            template_str += "\n\n## Conversation Segment\n" + conv.str();
        }

        std::vector<Message> msgs = {
            {"system", "You are a memory distillation agent. Your goal is to compress raw session logs into meaningful daily summaries.", "", "", ""},
            {"user", template_str, "", "", ""}
        };

        // Use dedicated distillation model/endpoint if configured
        std::string provider = Config::instance().memory_distillation_provider();
        std::string model = Config::instance().memory_distillation_model();
        std::string endpoint = Config::instance().memory_distillation_endpoint();

        LLMResponse result = llm_fn_(msgs, "[]", [](const AgentEvent&){}, model, endpoint, provider);
        
        if (!result.content.empty()) {
            context_.memory().append_daily_log(result.content);
            session.last_distilled_token_count = session.estimate_tokens();
            spdlog::info("L1 -> L2 distillation completed");
        }
    }

    bool consolidate_memory(Session& session, int start, int end) {
        spdlog::info("Consolidating memory for session: {} (Range: {}-{})", session.key, start, end);

        if (end - start < 2) return false;

        std::stringstream conv;
        for (int i = start; i < end; ++i) {
            conv << "[" << session.messages[i].role << "]: " << session.messages[i].content << "\n";
        }

        std::string current_mem = context_.memory().read_long_term();

        std::string prompt = "Process this conversation and return a JSON object with:\n"
            "1. \"history_entry\": A summary paragraph (2-5 sentences) of key events.\n"
            "2. \"memory_update\": Updated long-term memory content. Add new facts or keep existing.\n\n"
            "## Current Memory\n" + current_mem + "\n\n"
            "## Conversation\n" + conv.str();

        std::vector<Message> msgs = {
            {"system", "You are a memory consolidation agent. Respond ONLY with valid JSON.", "", "", ""},
            {"user", prompt, "", "", ""}
        };

        // Use dedicated distillation model/endpoint if configured
        std::string provider = Config::instance().memory_distillation_provider();
        std::string model = Config::instance().memory_distillation_model();
        std::string endpoint = Config::instance().memory_distillation_endpoint();

        LLMResponse result = llm_fn_(msgs, "[]", [](const AgentEvent&){}, model, endpoint, provider);

        try {
            size_t start_json = result.content.find('{');
            size_t end_json = result.content.rfind('}');
            if (start_json != std::string::npos && end_json != std::string::npos) {
                std::string json_text = result.content.substr(start_json, end_json - start_json + 1);
                simdjson::dom::parser parser;
                simdjson::dom::element res;
                auto padded = simdjson::padded_string(json_text);
                auto error = parser.parse(padded).get(res);

                if (!error) {
                    simdjson::dom::element history_val;
                    if (!res["history_entry"].get(history_val)) {
                        std::string_view entry_sv;
                        std::string entry;
                        if (!history_val.get(entry_sv)) entry = std::string(entry_sv);
                        else entry = simdjson::to_string(history_val);
                        context_.memory().index_session_message(session.key, "system", "--- CONSOLIDATED ---\n" + entry);
                    }
                    simdjson::dom::element memory_val;
                    if (!res["memory_update"].get(memory_val)) {
                        std::string_view update_sv;
                        std::string update;
                        if (!memory_val.get(update_sv)) update = std::string(update_sv);
                        else update = simdjson::to_string(memory_val);
                        context_.memory().write_long_term(update);
                    }
                    spdlog::info("Memory consolidated successfully for date: {}", session.last_consolidation_date);
                    return true;
                } else {
                    spdlog::error("simdjson parse error in consolidate_memory");
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Exception in consolidate_memory: {}", e.what());
        }
        return false;
    }

    ContextBuilder& context() { return context_; }

private:
    ContextBuilder context_;
    LLMCallFn llm_fn_;
    EmbeddingFn embed_fn_;
    int max_iterations_;
    std::map<std::string, std::shared_ptr<Tool>> tools_;

    static std::string json_escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    }

    static std::string current_time_hhmm() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&t);
        char buf[8];
        std::strftime(buf, sizeof(buf), "%H:%M", &tm);
        return buf;
    }

    static std::string current_date() {
        return date_at_offset(0);
    }

    static std::string yesterday_date() {
        return date_at_offset(-1);
    }

    static std::string date_at_offset(int days_offset) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now + std::chrono::hours(24 * days_offset));
        std::tm tm = *std::localtime(&t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        return buf;
    }

    static std::string current_timestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }
};
