#pragma once
// SubagentManager — mirrors nanobot/nanobot/agent/subagent.py
// Manages background agent tasks.

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <future>
#include <spdlog/spdlog.h>

#include "loop.hpp"
#include "session.hpp"
#include "../tools/terminal.hpp"
#include "../tools/file.hpp"
#include "../tools/web.hpp"

class SubagentManager {
public:
    SubagentManager(const std::string& workspace, LLMCallFn llm_fn)
        : workspace_(workspace), llm_fn_(llm_fn) {}

    std::string spawn(const std::string& task, const std::string& label, const std::string& session_id) {
        std::string task_id = "sub_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()).substr(10);
        std::string display_label = label.empty() ? (task.length() > 30 ? task.substr(0, 30) + "..." : task) : label;

        // Run in a background thread
        std::thread([this, task, display_label, session_id, task_id]() {
            run_subagent(task, display_label, session_id, task_id);
        }).detach();

        return "Subagent [" + display_label + "] started (id: " + task_id + "). I'll notify you when it completes.";
    }

private:
    std::string workspace_;
    LLMCallFn llm_fn_;

    void run_subagent(const std::string& task, const std::string& label, const std::string& session_id, const std::string& task_id) {
        spdlog::info("Subagent [{}] starting task: {}", task_id, label);

        // Subagents have a focused system prompt and limited tools
        // In nanobot, they don't have SpawnTool or MessageTool
        
        // Setup a local loop for the subagent
        AgentLoop sub_loop(workspace_, llm_fn_, /*max_iterations=*/15);
        
        // Register tools (excluding spawn/message)
        sub_loop.register_tool("terminal",   std::make_shared<TerminalTool>());
        sub_loop.register_tool("read_file",  std::make_shared<ReadFileTool>());
        sub_loop.register_tool("write_file", std::make_shared<WriteFileTool>());
        sub_loop.register_tool("web_search", std::make_shared<WebSearchTool>());
        sub_loop.register_tool("web_fetch",  std::make_shared<WebFetchTool>());

        // Create a fake session for the subagent
        Session sub_session;
        sub_session.key = "subagent:" + task_id;
        
        // We need a way to "announce" the result back to the main agent.
        // In nanobot, it publishes an InboundMessage to the bus.
        // Here, we can simulate this by appending a system message to the ORIGINAL session.
        
        std::string final_result;
        sub_loop.process(task, sub_session, [&](const AgentEvent& ev) {
            // We only care about the final tokens or completion
            if (ev.type == "token") {
                // Ignore tokens for background tasks
            } else if (ev.type == "done") {
                // Done
            }
        });

        // The processed session now has the assistant's final response as the last message
        if (!sub_session.messages.empty() && sub_session.messages.back().role == "assistant") {
            final_result = sub_session.messages.back().content;
        } else {
            final_result = "Task completed but no final response was generated.";
        }

        spdlog::info("Subagent [{}] completed", task_id);
        announce_result(label, task, final_result, session_id);
    }

    void announce_result(const std::string& label, const std::string& task, const std::string& result, const std::string& session_id) {
        // In a real system with a message bus, we'd send an event.
        // For now, we'll log it. In the future, we could have the API poll for background results
        // or push via WebSocket.
        // As a simple "parity" measure, we can append a system notification to the session log.
        
        SessionManager sm(workspace_);
        auto session = sm.get_or_create(session_id);
        
        std::string announce = "[Subagent '" + label + "' completed]\n\nTask: " + task + "\n\nResult:\n" + result;
        session.add_message("system", announce);
        sm.save(session);
        
        spdlog::info("Subagent result announced to session: {}", session_id);
    }
};
