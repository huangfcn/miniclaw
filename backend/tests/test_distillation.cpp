#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include "agent/loop.hpp"
#include "agent/memory_index.hpp"

namespace fs = std::filesystem;

// Mock LLM for distillation
LLMResponse mock_distill_llm(
    const std::vector<Message>& messages, 
    const std::string& tools_json, 
    EventCallback on_event,
    const std::string& model,
    const std::string& endpoint,
    const std::string& provider
) {
    LLMResponse resp;
    
    // Check if it's L1 -> L2 distillation prompt
    bool is_l1_l2 = false;
    for (const auto& m : messages) {
        if (m.content.find("Summarize the following conversation") != std::string::npos ||
            m.content.find("Daily Log") != std::string::npos) {
            is_l1_l2 = true;
            break;
        }
    }

    // Check if it's L2 -> L3 consolidation prompt
    bool is_l2_l3 = false;
    for (const auto& m : messages) {
        if (m.content.find("memory_update") != std::string::npos) {
            is_l2_l3 = true;
            break;
        }
    }

    if (is_l1_l2) {
        resp.content = "MOCK_DAILY_SUMMARY: User talked about testing distillation.";
    } else if (is_l2_l3) {
        resp.content = R"({
            "history_entry": "Tested memory distillation layers successfully.",
            "memory_update": "# Long Term Memory\n- Distillation process works."
        })";
    } else {
        resp.content = "I am a helpful assistant.";
    }

    return resp;
}

std::string get_today_date() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%d");
    return ss.str();
}

int main() {
    std::string workspace = "./test_ws";
    fs::create_directories(workspace);
    fs::create_directories(fs::path(workspace) / "memory");

    auto mock_embed_fn = [](const std::string&) -> std::vector<float> { 
        return std::vector<float>(1024, 0.1f); 
    };

    AgentLoop loop(workspace, mock_distill_llm, mock_embed_fn, 5);

    Session session;
    session.key = "distill_test_session";
    session.add_message("user", "Hello, let's test distillation.");
    session.add_message("assistant", "Sure, I'm ready.");
    session.add_message("user", "This is some important info for L2.");
    session.add_message("assistant", "Noted for L2.");

    std::cout << "Testing L1 -> L2 Distillation..." << std::endl;
    loop.distill_l1_to_l2(session, AgentLoop::DistillationEvent::PERIODIC);

    // Verify L2 file exists
    std::string today = get_today_date();
    fs::path daily_log = fs::path(workspace) / "memory" / (today + ".md");
    assert(fs::exists(daily_log));

    std::ifstream f2(daily_log);
    std::string l2_content((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    std::cout << "L2 Content:\n" << l2_content << std::endl;
    assert(l2_content.find("MOCK_DAILY_SUMMARY") != std::string::npos);
    f2.close();

    std::cout << "Testing L2 -> L3 Consolidation..." << std::endl;
    // For consolidation, we need some messages since last consolidated
    session.add_message("user", "Now let's move to permanent memory.");
    session.add_message("assistant", "Finalizing memory store.");
    
    loop.consolidate_memory(session);

    // Verify L3 file exists
    fs::path l3_memory = fs::path(workspace) / "memory" / "MEMORY.md";
    assert(fs::exists(l3_memory));

    std::ifstream f3(l3_memory);
    std::string l3_content((std::istreambuf_iterator<char>(f3)), std::istreambuf_iterator<char>());
    std::cout << "L3 Content:\n" << l3_content << std::endl;
    assert(l3_content.find("Distillation process works") != std::string::npos);
    f3.close();

    std::cout << "\n✅ Memory Distillation Test PASSED!" << std::endl;

    // Cleanup
    fs::remove_all(workspace);

    return 0;
}
