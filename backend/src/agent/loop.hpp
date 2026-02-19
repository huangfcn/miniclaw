#pragma once
// AgentLoop — mirrors nanobot/nanobot/agent/loop.py
// Core ReAct iteration: call LLM → parse tool calls → execute → repeat

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <sstream>
#include <regex>
#include <spdlog/spdlog.h>

#include "context.hpp"
#include "session.hpp"
#include "../tools/tool.hpp"
#include <fiber.hpp>

// Event types streamed back to the caller
struct AgentEvent {
    std::string type;    // "token" | "tool_start" | "tool_end" | "done" | "error"
    std::string content;
};

using EventCallback = std::function<void(const AgentEvent&)>;

// LLMCallFn is now "synchronous" from the point of view of the fiber.
// It will suspend the fiber and resume when metadata/tokens arrive.
using LLMCallFn = std::function<std::string(
    const std::vector<Message>& messages,
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

    // Register a tool
    void register_tool(const std::string& name, std::shared_ptr<Tool> tool) {
        tools_[name] = std::move(tool);
    }

    std::shared_ptr<Tool> get_tool(const std::string& name) {
        if (tools_.count(name)) return tools_[name];
        return nullptr;
    }


    // Process a single user message, using the provided session for history.
    void process(
        const std::string& user_message,
        Session& session,
        EventCallback on_event
    ) {
        // 1. Append user message to history file (simple log)
        context_.memory().append_history(
            "[" + current_timestamp() + "] USER: " + user_message
        );

        // 2. Build message list from session history + current message
        // history passed to context_.build_messages should be the recent window
        int memory_window = 20; // Default window
        std::vector<Message> history = session.messages;
        if (history.size() > memory_window) {
            history.erase(history.begin(), history.end() - memory_window);
        }

        std::vector<Message> messages = context_.build_messages(history, user_message);

        int iteration = 0;
        bool tool_used = false;

        while (iteration < max_iterations_) {
            ++iteration;

            std::string response = llm_fn_(messages, on_event);

            if (response.empty() || response.rfind("Error", 0) == 0) {
                on_event({"error", response.empty() ? "LLM returned empty response" : response});
                break;
            }

            std::regex tool_re(R"DELIM(<tool name="([^"]+)">([\s\S]*?)</tool>)DELIM");
            std::smatch match;

            if (std::regex_search(response, match, tool_re)) {
                tool_used = true;
                std::string tool_name  = match[1].str();
                std::string tool_input = trim(match[2].str());

                on_event({"tool_start", tool_name + ": " + tool_input});
                
                context_.memory().append_history(
                    "[" + current_timestamp() + "] TOOL " + tool_name + ": " + tool_input
                );

                std::string output;
                if (tools_.count(tool_name)) {
                    output = tools_.at(tool_name)->execute(tool_input);
                } else {
                    output = "Error: unknown tool '" + tool_name + "'";
                }

                on_event({"tool_end", output});

                context_.memory().append_history(
                    "[" + current_timestamp() + "] TOOL_OUTPUT: " + output.substr(0, 500)
                );

                messages.push_back({"assistant", response});
                messages.push_back({"user", "Tool Output:\n" + output + "\n\nReflect on the result and decide next steps."});

            } else {
                // Final answer
                context_.memory().append_history(
                    "[" + current_timestamp() + "] ASSISTANT: " + response
                );
                
                // Update session
                session.add_message("user", user_message);
                session.add_message("assistant", response);

                if (session.messages.size() - session.last_consolidated > 10) {
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

    // Port of nanobot's _consolidate_memory
    void consolidate_memory(Session& session) {
        spdlog::info("Consolidating memory for session: {}", session.key);
        
        // 1. Prepare conversation slice for the LLM
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
            {"system", "You are a memory consolidation agent. Respond ONLY with valid JSON."},
            {"user", prompt}
        };

        // Call LLM synchronously for consolidation (it will block the fiber)
        std::string result_str = llm_fn_(msgs, [](const AgentEvent&){});
        
        try {
            // Find JSON in response
            size_t start_json = result_str.find('{');
            size_t end_json = result_str.rfind('}');
            if (start_json != std::string::npos && end_json != std::string::npos) {
                std::string json_text = result_str.substr(start_json, end_json - start_json + 1);
                auto res = json::parse(json_text);
                
                if (res.contains("history_entry")) {
                    auto& val = res["history_entry"];
                    std::string entry = val.is_string() ? val.get<std::string>() : val.dump();
                    context_.memory().append_history("--- CONSOLIDATED ---\n" + entry);
                }
                if (res.contains("memory_update")) {
                    auto& val = res["memory_update"];
                    std::string update = val.is_string() ? val.get<std::string>() : val.dump();
                    context_.memory().write_long_term(update);
                }
                session.last_consolidated = end;
                spdlog::info("Memory consolidated successfully");
            }
        } catch (const std::exception& e) {
            spdlog::error("Memory consolidation failed: {}", e.what());
        }
    }

    ContextBuilder& context() { return context_; }

private:
    ContextBuilder context_;
    LLMCallFn llm_fn_;
    int max_iterations_;
    std::map<std::string, std::shared_ptr<Tool>> tools_;

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
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
