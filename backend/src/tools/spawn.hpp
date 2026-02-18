#pragma once
// SpawnTool — mirrors nanobot/nanobot/agent/tools/spawn.py
// Creates a background subagent task.

#include "tool.hpp"
#include "../agent/subagent.hpp"
#include <string>
#include <memory>

class SpawnTool : public Tool {
public:
    SpawnTool(SubagentManager& manager) : manager_(manager) {}

    std::string name() const override { return "spawn"; }
    std::string description() const override { return "Spawn a subagent to handle a task in the background"; }

    void set_context(const std::string& session_id) {
        session_id_ = session_id;
    }

    std::string execute(const std::string& input) override {
        // Input format: task (and optional label if we parsed it, but simple XML tool arg is just string)
        // For now, treat whole input as the task
        std::string task = input;
        std::string label = ""; // Simple version
        
        return manager_.spawn(task, label, session_id_);
    }

private:
    SubagentManager& manager_;
    std::string session_id_;
};
