#pragma once
#include "tool.hpp"
#include "../agent/cron_service.hpp"
#include <simdjson.h>
#include <map>
#include <sstream>
#include <ctime>

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
                "description": "Manage background tasks (add, remove, list).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "action": {
                            "type": "string",
                            "enum": ["add", "remove", "list"],
                            "description": "The action to perform."
                        },
                        "schedule": {
                            "type": "string",
                            "description": "Cron expression or 'every 10s' (required for 'add')."
                        },
                        "task": {
                            "type": "string",
                            "description": "Description of the task to perform (required for 'add')."
                        },
                        "id": {
                            "type": "string",
                            "description": "Job ID to remove (required for 'remove')."
                        }
                    },
                    "required": ["action"]
                }
            }
        })";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it_action = args.find("action");
        std::string action = (it_action != args.end()) ? it_action->second : "add"; // Default to add for backward compatibility

        if (action == "add") {
            auto it_sched = args.find("schedule");
            auto it_task = args.find("task");
            if (it_sched == args.end() || it_task == args.end()) {
                return "Error: missing schedule or task for 'add' action";
            }

            std::string id = CronService::instance().add_job(it_sched->second, it_task->second);
            if (id.empty()) {
                return "Error: failed to schedule job (invalid syntax?)";
            }
            return "Success: Job scheduled with ID " + id + ". Please inform the user that their task has been scheduled.";
        } else if (action == "remove") {
            auto it_id = args.find("id");
            if (it_id == args.end()) {
                return "Error: missing id for 'remove' action";
            }
            if (CronService::instance().remove_job(it_id->second)) {
                return "Success: Job " + it_id->second + " removed.";
            } else {
                return "Error: job ID " + it_id->second + " not found.";
            }
        } else if (action == "list") {
            auto jobs = CronService::instance().list_jobs();
            if (jobs.empty()) {
                return "No active cron jobs.";
            }
            std::stringstream ss;
            ss << "Active Cron Jobs:\n";
            for (const auto& job : jobs) {
                std::string time_str = std::ctime(&job.next_run);
                if (!time_str.empty() && time_str.back() == '\n') time_str.pop_back();
                ss << "- ID: " << job.id << ", Task: " << job.task_description << ", Schedule: " << job.cron_expr << ", Next run: " << time_str << "\n";
            }
            return ss.str();
        }

        return "Error: invalid action '" + action + "'";
    }
};
