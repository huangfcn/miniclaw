#include "agent.hpp"
#include "agent/subagent.hpp"

#include <spdlog/spdlog.h>
#include <sstream>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "tools/terminal.hpp"
#include "tools/file.hpp"
#include "tools/web.hpp"
#include "tools/spawn.hpp"

using json = nlohmann::json;

Agent::~Agent() = default;

Agent::Agent() {
    api_key_   = std::getenv("OPENAI_API_KEY")  ? std::getenv("OPENAI_API_KEY")  : "";
    api_base_  = std::getenv("OPENAI_API_BASE") ? std::getenv("OPENAI_API_BASE") : "api.openai.com";
    model_     = std::getenv("OPENAI_MODEL")    ? std::getenv("OPENAI_MODEL")    : "gpt-4-turbo";
    workspace_ = std::getenv("WORKSPACE_DIR")   ? std::getenv("WORKSPACE_DIR")   : ".";

    // Build the LLM call function (captures this)
    LLMCallFn llm_fn = [this](
        const std::vector<Message>& messages,
        std::function<void(const std::string&)> on_token
    ) -> std::string {
        return call_llm(messages, on_token);
    };

    loop_ = std::make_unique<AgentLoop>(workspace_, llm_fn, /*max_iterations=*/10);
    sessions_ = std::make_unique<SessionManager>(workspace_);
    subagents_ = std::make_unique<SubagentManager>(workspace_, llm_fn);

    // Register built-in tools
    loop_->register_tool("terminal",   std::make_shared<TerminalTool>());
    loop_->register_tool("read_file",  std::make_shared<ReadFileTool>());
    loop_->register_tool("write_file", std::make_shared<WriteFileTool>());
    loop_->register_tool("web_search", std::make_shared<WebSearchTool>());
    loop_->register_tool("web_fetch",  std::make_shared<WebFetchTool>());
    loop_->register_tool("spawn",      std::make_shared<SpawnTool>(*subagents_));

    spdlog::info("Agent initialized: model={} workspace={}", model_, workspace_);
}

void Agent::run(
    const std::string& user_message,
    const std::string& session_id,
    AgentEventCallback on_event
) {
    auto session = sessions_->get_or_create(session_id);
    
    // Update tools that need session context
    auto spawn_tool = std::dynamic_pointer_cast<SpawnTool>(loop_->get_tool("spawn"));
    if (spawn_tool) spawn_tool->set_context(session_id);

    loop_->process(user_message, session, on_event);
    sessions_->save(session);
}

std::string Agent::call_llm(
    const std::vector<Message>& messages,
    std::function<void(const std::string&)> on_token
) {
    // Build JSON payload
    json j_messages = json::array();
    for (const auto& m : messages) {
        j_messages.push_back({{"role", m.role}, {"content", m.content}});
    }
    json payload = {
        {"model",    model_},
        {"messages", j_messages},
        {"stream",   true}
    };

    httplib::Client cli("https://" + api_base_);
    cli.set_bearer_token_auth(api_key_);
    cli.set_read_timeout(300, 0);

    std::string full_response;

    httplib::Request req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.headers = {{"Content-Type", "application/json"}};
    req.body = payload.dump();
    req.content_receiver = [&](const char* data, size_t len, uint64_t /*off*/, uint64_t /*total*/) {
        std::string chunk(data, len);
        std::istringstream ss(chunk);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.rfind("data: ", 0) != 0) continue;
            std::string json_str = line.substr(6);
            if (json_str == "[DONE]") continue;
            try {
                auto j = json::parse(json_str);
                if (j["choices"][0]["delta"].contains("content")) {
                    std::string tok = j["choices"][0]["delta"]["content"];
                    on_token(tok);
                    full_response += tok;
                }
            } catch (...) {}
        }
        return true;
    };

    auto res = cli.send(req);

    if (!res || res->status != 200) {
        int code = res ? res->status : 0;
        spdlog::error("LLM HTTP error: {}", code);
        return "Error: LLM call failed (HTTP " + std::to_string(code) + ")";
    }

    return full_response;
}
