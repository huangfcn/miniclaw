#pragma once
// SpawnTool — mirrors nanobot/nanobot/agent/tools/spawn.py
// Creates a background subagent task.

#include <fiber.hpp>
#include <string>
#include <memory>

class SpawnTool : public Tool {
public:
    SpawnTool(SubagentManager& manager) : manager_(manager) {}

    std::string name() const override { return "spawn"; }
    std::string description() const override { return "Spawn a subagent to handle a task in the background"; }

    std::string execute(const std::string& input) override {
        // Retrieve session_id from current fiber's local storage (Index 0)
        auto* fiber = fib::Fiber::self();
        uint64_t session_ptr_val = fiber->getLocalData(0);
        
        std::string session_id;
        if (session_ptr_val) {
            session_id = *reinterpret_cast<std::string*>(session_ptr_val);
        } else {
            return "Error: Session context missing in fiber task.";
        }

        // Input format: treat whole input as the task
        std::string task = input;
        std::string label = ""; 
        
        return manager_.spawn(task, label, session_id);
    }

private:
    SubagentManager& manager_;
};
