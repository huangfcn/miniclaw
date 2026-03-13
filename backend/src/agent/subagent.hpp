#pragma once
// SubagentManager — mirrors nanobot/nanobot/agent/subagent.py
// Manages background agent tasks.

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <spdlog/spdlog.h>
#include "fiber_pool.hpp"

#include "loop.hpp"
#include "session.hpp"
#include "../tools/terminal.hpp"
#include "../tools/file.hpp"
#include "../tools/web.hpp"
#include "agent.hpp"

class SubagentManager {
public:
    SubagentManager(const std::string& workspace, LLMCallFn llm_fn, EmbeddingFn embed_fn)
        : workspace_(workspace), llm_fn_(llm_fn), embed_fn_(embed_fn) {}

    std::string spawn(const std::string& task, const std::string& label, const std::string& session_id) {
        std::string task_id = "sub_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()).substr(10);
        std::string display_label = label.empty() ? (task.length() > 30 ? task.substr(0, 30) + "..." : task) : label;

        FiberPool::instance().spawn([this, task, display_label, session_id, task_id]() {
            spdlog::info("Subagent [{}] starting task: {}", task_id, display_label);
            
            // We need to set the session context for any tool calls emitted by the subagent
            // The Agent::run would normally do this, but for background subagents we do it here.
            // However, t_session_id is static thread_local in agent.cpp. 
            // We'll trust that subagents don't need nesting of sessions for now or we might need 
            // a better way to propagate it.
            
            try {
                AgentLoop sub_loop(workspace_, llm_fn_, embed_fn_, 15);
                
                sub_loop.register_tool("exec",       std::make_shared<TerminalTool>());
                sub_loop.register_tool("read_file",  std::make_shared<ReadFileTool>());
                sub_loop.register_tool("write_file", std::make_shared<WriteFileTool>());
                sub_loop.register_tool("edit_file",  std::make_shared<EditFileTool>());
                sub_loop.register_tool("list_dir",   std::make_shared<ListDirTool>());
                sub_loop.register_tool("web_search", std::make_shared<WebSearchTool>());
                sub_loop.register_tool("web_fetch",  std::make_shared<WebFetchTool>());

                Session sub_session;
                sub_session.key = "subagent:" + task_id;
                
                sub_loop.process(task, sub_session, [](const AgentEvent& ev) {}, "", session_id);

                std::string final_result;
                if (!sub_session.messages.empty() && sub_session.messages.back().role == "assistant") {
                    final_result = sub_session.messages.back().content;
                } else {
                    final_result = "Task completed but no final response was generated.";
                }

                spdlog::info("Subagent [{}] completed", task_id);
                announce_result(display_label, task, final_result, session_id);
            } catch (const std::exception& e) {
                spdlog::error("Subagent [{}] exception: {}", task_id, e.what());
            } catch (...) {
                spdlog::error("Subagent [{}] unknown exception", task_id);
            }
        });

        return "Subagent [" + display_label + "] started (id: " + task_id + "). I'll notify you when it completes.";
    }

    void announce_result(const std::string& label, const std::string& task, const std::string& result, const std::string& session_id) {
        SessionManager sm(workspace_);
        auto session = sm.get_or_create(session_id);
        
        std::string announce = "[Subagent '" + label + "' completed]\n\nTask: " + task + "\n\nResult:\n" + result;
        session.add_message("system", announce);
        sm.save(session);
        
        spdlog::info("Subagent result announced to session: {}", session_id);
    }

private:
    std::string workspace_;
    LLMCallFn llm_fn_;
    EmbeddingFn embed_fn_;
};
