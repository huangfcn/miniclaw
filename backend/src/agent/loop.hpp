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
#include <fiber.hpp>
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
    EventCallback on_event
)>;

class AgentLoop {
public:
    AgentLoop(
        const std::string& workspace,
        LLMCallFn llm_fn,
        int max_iterations = 10
    )
        : context_(workspace)
        , llm_fn_(std::move(llm_fn))
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
        for (const auto& [name, tool] : tools_) {
            if (!first) result += ",";
            result += tool->schema();
            first = false;
        }
        result += "]";
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
        context_.memory().append_history(
            "[" + current_timestamp() + "] USER: " + user_message
        );

        int memory_window = 20;
        std::vector<Message> history = session.messages;
        if (history.size() > memory_window) {
            history.erase(history.begin(), history.end() - memory_window);
        }

        std::vector<Message> messages = context_.build_messages(history, user_message, channel, chat_id);

        const std::string tools_json = build_tools_json();
        int iteration = 0;

        while (iteration < max_iterations_) {
            ++iteration;

            LLMResponse response = llm_fn_(messages, tools_json, on_event);
            spdlog::debug("AgentLoop iteration {}: has_tool_calls={} content_len={}",
                iteration, response.has_tool_calls(), response.content.size());

            if (response.content.find("Error") == 0 && !response.has_tool_calls()) {
                on_event({"error", response.content});
                break;
            }

            if (response.has_tool_calls()) {
                // Stream any thinking content to the user
                if (!response.content.empty()) {
                    on_event({"token", response.content});
                }

                // Append the assistant message with tool_calls
                messages.push_back(make_assistant_tool_call_message(response.content, response.tool_calls));

                // Execute each tool call
                for (const auto& tc : response.tool_calls) {
                    spdlog::info("Tool call: {}({})", tc.name, tc.arguments_json.substr(0, 200));
                    on_event({"tool_start", tc.name + ": " + tc.arguments_json});

                    context_.memory().append_history(
                        "[" + current_timestamp() + "] TOOL " + tc.name + ": " + tc.arguments_json
                    );

                    std::string output;
                    if (tools_.count(tc.name)) {
                        auto args = parse_arguments(tc.arguments_json);
                        output = tools_.at(tc.name)->execute(args);
                    } else {
                        output = "Error: unknown tool '" + tc.name + "'";
                    }

                    on_event({"tool_end", output});
                    context_.memory().append_history(
                        "[" + current_timestamp() + "] TOOL_OUTPUT: " + output.substr(0, 500)
                    );

                    messages.push_back(make_tool_result_message(tc.id, tc.name, output));
                }

            } else {
                // Final answer
                context_.memory().append_history(
                    "[" + current_timestamp() + "] ASSISTANT: " + response.content
                );

                session.add_message("user", user_message);
                session.add_message("assistant", response.content);

                if (session.messages.size() - session.last_consolidated >
                        Config::instance().memory_consolidation_threshold()) {
                    consolidate_memory(session);
                }

                on_event({"done", ""});
                break;
            }
        }

        if (iteration >= max_iterations_) {
            on_event({"error", "Max iterations reached"});
        }
    }

    void consolidate_memory(Session& session) {
        spdlog::info("Consolidating memory for session: {}", session.key);

        int start = session.last_consolidated;
        int end = session.messages.size();
        if (end - start < 2) return;

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

        LLMResponse result = llm_fn_(msgs, "[]", [](const AgentEvent&){});

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
                        context_.memory().append_history("--- CONSOLIDATED ---\n" + entry);
                    }
                    simdjson::dom::element memory_val;
                    if (!res["memory_update"].get(memory_val)) {
                        std::string_view update_sv;
                        std::string update;
                        if (!memory_val.get(update_sv)) update = std::string(update_sv);
                        else update = simdjson::to_string(memory_val);
                        context_.memory().write_long_term(update);
                    }
                    session.last_consolidated = end;
                    spdlog::info("Memory consolidated successfully");
                } else {
                    spdlog::error("simdjson parse error in consolidate_memory");
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Exception in consolidate_memory: {}", e.what());
        }
    }

    ContextBuilder& context() { return context_; }

private:
    ContextBuilder context_;
    LLMCallFn llm_fn_;
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

    static std::string current_timestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }
};
