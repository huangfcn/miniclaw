#pragma once
// SpawnTool — creates a background subagent task.

#include <fiber.hpp>
#include <string>
#include <memory>
#include <map>

class SpawnTool : public Tool {
public:
    SpawnTool(SubagentManager& manager) : manager_(manager) {}

    std::string name() const override { return "spawn"; }
    std::string description() const override {
        return "Spawn a subagent to handle a complex task in the background.";
    }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"spawn","description":"Spawn a subagent to handle a complex task in the background.","parameters":{"type":"object","properties":{"task":{"type":"string","description":"Full description of the task for the subagent"},"label":{"type":"string","description":"Optional short label for the task"}},"required":["task"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto task_it = args.find("task");
        if (task_it == args.end()) return "Error: missing 'task' argument";
        std::string label = "";
        auto label_it = args.find("label");
        if (label_it != args.end()) label = label_it->second;

        // Retrieve session_id from current fiber's local storage (Index 0)
        auto* fiber = fib::Fiber::self();
        uint64_t session_ptr_val = fiber->getLocalData(0);
        std::string session_id;
        if (session_ptr_val) {
            session_id = *reinterpret_cast<std::string*>(session_ptr_val);
        } else {
            return "Error: Session context missing in fiber task.";
        }

        return manager_.spawn(task_it->second, label, session_id);
    }

    std::string execute(const std::string& input) override {
        std::map<std::string, std::string> args = {{"task", input}};
        return execute(args);
    }

private:
    SubagentManager& manager_;
};
