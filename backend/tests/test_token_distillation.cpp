#include <iostream>
#include <fstream>
#include <filesystem>
#include "src/agent/session.hpp"
#include "src/config.hpp"

namespace fs = std::filesystem;

int main() {
    std::cout << "Testing token-based distillation configuration..." << std::endl;
    
    // Test configuration loading
    Config config;
    
    std::cout << "Default L1 distillation trigger: " << config.memory_l1_distillation_trigger() << std::endl;
    std::cout << "Default L1 token threshold: " << config.memory_l1_token_threshold() << std::endl;
    std::cout << "Default L1 message threshold: " << config.memory_l1_to_l2_threshold() << std::endl;
    
    // Test session with token tracking
    Session session;
    session.key = "test_session";
    
    // Add some messages
    for (int i = 0; i < 10; i++) {
        session.add_message("user", "This is test message number " + std::to_string(i) + " with some content to make it longer.");
        session.add_message("assistant", "This is the assistant response to message " + std::to_string(i) + " with detailed information.");
    }
    
    std::cout << "Session messages: " << session.messages.size() << std::endl;
    std::cout << "Estimated tokens: " << session.estimate_tokens() << std::endl;
    std::cout << "Last distilled token count: " << session.last_distilled_token_count << std::endl;
    
    // Test saving and loading
    SessionManager manager(".");
    manager.save(session);
    
    Session loaded = manager.load(session.key);
    std::cout << "Loaded session last distilled token count: " << loaded.last_distilled_token_count << std::endl;
    
    std::cout << "Test completed successfully!" << std::endl;
    return 0;
}