#pragma once
// SubagentManager — mirrors nanobot/nanobot/agent/subagent.py
// Manages background agent tasks.

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <spdlog/spdlog.h>

#include "loop.hpp"
#include "session.hpp"
#include "../tools/terminal.hpp"
#include "../tools/file.hpp"
#include "../tools/web.hpp"
#include "agent.hpp"

class SubagentManager {
public:
    SubagentManager(const std::string& workspace, LLMCallFn llm_fn)
        : workspace_(workspace), llm_fn_(llm_fn) {}

    std::string spawn(const std::string& task, const std::string& label, const std::string& session_id) {
        std::string task_id = "sub_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()).substr(10);
        std::string display_label = label.empty() ? (task.length() > 30 ? task.substr(0, 30) + "..." : task) : label;

        // Run in the fiber scheduler thread
        spawn_in_fiber([this, task, display_label, session_id, task_id]() {
            run_subagent(task, display_label, session_id, task_id);
        });

        return "Subagent [" + display_label + "] started (id: " + task_id + "). I'll notify you when it completes.";
    }

private:
    std::string workspace_;
    LLMCallFn llm_fn_;

    struct SubData {
        SubagentManager* self;
        std::string task;
        std::string label;
        std::string session_id;
        std::string task_id;
    };

    void run_subagent(const std::string& task, const std::string& label, const std::string& session_id, const std::string& task_id) {
        spdlog::info("Subagent [{}] starting task: {}", task_id, label);
        
        auto* data = new SubData{this, task, label, session_id, task_id};

        fiber_create([](void* arg) -> void* {
            auto* d = (SubData*)arg;
            
            // Set session_id in Fiber Local Storage (Index 0)
            auto* fiber = fib::Fiber::self();
            fiber->setLocalData(0, reinterpret_cast<uint64_t>(&d->session_id));

            try {
                spdlog::debug("Subagent [{}] loop start", d->task_id);
                // Setup a local loop for the subagent
                AgentLoop sub_loop(d->self->workspace_, d->self->llm_fn_, 15);
                
                // Register tools
                sub_loop.register_tool("exec",       std::make_shared<TerminalTool>());
                sub_loop.register_tool("read_file",  std::make_shared<ReadFileTool>());
                sub_loop.register_tool("write_file", std::make_shared<WriteFileTool>());
                sub_loop.register_tool("edit_file",  std::make_shared<EditFileTool>());
                sub_loop.register_tool("list_dir",   std::make_shared<ListDirTool>());
                sub_loop.register_tool("web_search", std::make_shared<WebSearchTool>());
                sub_loop.register_tool("web_fetch",  std::make_shared<WebFetchTool>());

                Session sub_session;
                sub_session.key = "subagent:" + d->task_id;
                
                std::string final_result;
                sub_loop.process(d->task, sub_session, [](const AgentEvent& ev) {});

                if (!sub_session.messages.empty() && sub_session.messages.back().role == "assistant") {
                    final_result = sub_session.messages.back().content;
                } else {
                    final_result = "Task completed but no final response was generated.";
                }

                spdlog::info("Subagent [{}] completed", d->task_id);
                d->self->announce_result(d->label, d->task, final_result, d->session_id);
            } catch (const std::exception& e) {
                spdlog::error("Subagent [{}] exception: {}", d->task_id, e.what());
            } catch (...) {
                spdlog::error("Subagent [{}] unknown exception", d->task_id);
            }
            delete d;
            return nullptr;
        }, data, NULL, 1024 * 1024); // 2MB stack
    }

    void announce_result(const std::string& label, const std::string& task, const std::string& result, const std::string& session_id) {
        SessionManager sm(workspace_);
        auto session = sm.get_or_create(session_id);
        
        std::string announce = "[Subagent '" + label + "' completed]\n\nTask: " + task + "\n\nResult:\n" + result;
        session.add_message("system", announce);
        sm.save(session);
        
        spdlog::info("Subagent result announced to session: {}", session_id);
    }
};
