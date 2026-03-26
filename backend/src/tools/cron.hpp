#pragma once
#include "tool.hpp"
#include "../agent/cron_service.hpp"
#include <simdjson.h>
#include <map>

class CronTool : public Tool {
public:
    std::string name() const override { return "cron"; }
    std::string description() const override {
        return "Schedule a periodic or delayed task. Use 'every Ns' for intervals or a standard cron expression.";
    }

    std::string schema() const override {
        return R"({
            "type": "function",
            "function": {
                "name": "cron",
                "description": "Schedule a background task.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "schedule": {
                            "type": "string",
                            "description": "Cron expression or 'every 10s'."
                        },
                        "task": {
                            "type": "string",
                            "description": "Description of the task to perform when triggered."
                        }
                    },
                    "required": ["schedule", "task"]
                }
            }
        })";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it_sched = args.find("schedule");
        auto it_task = args.find("task");
        if (it_sched == args.end() || it_task == args.end()) {
            return "Error: missing schedule or task";
        }

        std::string id = CronService::instance().add_job(it_sched->second, it_task->second);
        if (id.empty()) {
            return "Error: failed to schedule job (invalid syntax?)";
        }
        return "Success: Job scheduled with ID " + id;
    }
};
