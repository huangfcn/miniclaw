#include <iostream>
#include <cassert>
#include <filesystem>
#include "agent/loop.hpp"
#include "tools/file.hpp"

// Mock LLM: on first call returns a native tool_call (write_file),
// on second call returns the final answer.
int g_llm_calls = 0;
LLMResponse mock_llm_fn(
    const std::vector<Message>& messages, 
    const std::string& tools_json, 
    EventCallback on_event,
    const std::string& model,
    const std::string& endpoint,
    const std::string& provider
) {
    g_llm_calls++;
    LLMResponse resp;
    if (g_llm_calls == 1) {
        // Simulate the LLM deciding to call write_file
        ToolCall tc;
        tc.id   = "call_test_001";
        tc.name = "write_file";
        tc.arguments_json = R"===({"path":"test_tool_use.txt","content":"PASSED"})===";
        resp.tool_calls.push_back(tc);
    } else {
        // Final answer
        resp.content = "The file has been written. DONE.";
    }
    return resp;
}

int main() {
    std::string workspace = ".";
    auto mock_embed_fn = [](const std::string&) -> std::vector<float> { return {}; };
    AgentLoop loop(workspace, mock_llm_fn, mock_embed_fn, 5);
    loop.register_tool("write_file", std::make_shared<WriteFileTool>());

    Session session;
    session.key = "test_session";

    std::cout << "Running Native Tool Call Test..." << std::endl;

    loop.process("Write 'PASSED' to test_tool_use.txt", session, [](const AgentEvent& ev) {
        if (ev.type == "tool_start") {
            std::cout << "[TOOL START] " << ev.content << std::endl;
        } else if (ev.type == "tool_end") {
            std::cout << "[TOOL END] " << ev.content << std::endl;
        }
    });

    // Verify expectations
    assert(g_llm_calls == 2);

    // Check if file exists and content is correct
    std::ifstream f("test_tool_use.txt");
    assert(f.is_open());
    std::string content;
    f >> content;
    assert(content == "PASSED");
    f.close();

    std::cout << "\n✅ Native Tool Call Test PASSED!" << std::endl;

    // Cleanup
    std::filesystem::remove("test_tool_use.txt");

    return 0;
}
