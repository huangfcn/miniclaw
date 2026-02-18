#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "agent/loop.hpp"
#include "agent/session.hpp"
#include "tools/tool.hpp"

// Event callback type (same as AgentLoop's EventCallback)
using AgentEventCallback = EventCallback;

class Agent {
public:
    Agent();

    // Process a user message, streaming events via callback.
    void run(
        const std::string& user_message,
        const std::string& session_id,
        AgentEventCallback on_event
    );

private:
    std::unique_ptr<AgentLoop> loop_;
    std::unique_ptr<SessionManager> sessions_;
    std::unique_ptr<SubagentManager> subagents_;

    // LLM HTTP call
    std::string call_llm(
        const std::vector<Message>& messages,
        std::function<void(const std::string&)> on_token
    );

    std::string api_key_;
    std::string api_base_;
    std::string model_;
    std::string workspace_;
};
