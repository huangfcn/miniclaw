#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "agent/agent_types.hpp"
using AgentEventCallback = EventCallback;
#include "agent/shutdown.hpp"

class AgentLoop;
class SessionManager;
class SubagentManager;

void init_spawn_system();
void spawn_in_fiber(std::function<void()> task);

class Agent {
public:
    Agent();
    virtual ~Agent();

    // Process a user message, streaming events via callback.
    void run(
        const std::string& user_message,
        const std::string& session_id,
        AgentEventCallback on_event,
        const std::string& channel = ""
    );

    // Access to internal components for testing
    // Access to internal components
    AgentLoop& loop();
    SessionManager& sessions();

private:
    std::unique_ptr<AgentLoop> loop_;
    std::unique_ptr<SessionManager> sessions_;
    std::unique_ptr<SubagentManager> subagents_;

    // Embedding call — fiber-blocking
    std::vector<float> embed(const std::string& text);

    // LLM HTTP call — fiber-blocking, returns structured LLMResponse
    LLMResponse call_llm(
        const std::vector<Message>& messages,
        const std::string& tools_json,
        AgentEventCallback on_event,
        const std::string& model = "",
        const std::string& endpoint = "",
        const std::string& provider = ""
    );

    std::string api_key_;
    std::string api_base_;
    std::string model_;
    std::string workspace_;
};
