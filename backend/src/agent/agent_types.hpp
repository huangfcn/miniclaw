#pragma once
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <memory>

// Forward declarations if needed
struct Message {
    std::string role;
    std::string content;
    std::string tool_call_id;
    std::string name;
    std::string tool_calls_json;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

struct LLMResponse {
    std::string content;
    std::vector<ToolCall> tool_calls;
    bool has_tool_calls() const { return !tool_calls.empty(); }
};

struct AgentEvent {
    std::string type;
    std::string content;
};

using EventCallback = std::function<void(const AgentEvent&)>;

using LLMCallFn = std::function<LLMResponse(
    const std::vector<Message>& messages,
    const std::string& tools_json,
    EventCallback on_event,
    const std::string& model,
    const std::string& endpoint,
    const std::string& provider
)>;

using EmbeddingFn = std::function<std::vector<float>(const std::string& text)>;
