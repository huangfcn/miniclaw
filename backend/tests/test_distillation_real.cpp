#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include "agent.hpp"
#include "agent/loop.hpp"
#include "agent/fiber_pool.hpp"
#include <spdlog/spdlog.h>
#include <atomic>

namespace fs = std::filesystem;

static std::atomic<bool> test_finished{false};

int main() {
    // 1. Load Configuration
    Config::instance().load();
    spdlog::set_level(spdlog::level::debug);
    
    std::cout << "Starting Real Distillation Test..." << std::endl;

    // 2. Initialize Fiber Internals
    FiberGlobalStartup();
    
    // 3. Initialize Agent & FiberPool (matching main.cpp behavior)
    static Agent agent;
    int threads = 1; // Single thread for test is fine
    FiberPool::instance().init(threads, &agent);

    init_spawn_system();

    // The test must run in a fiber for curl/async support
    spawn_in_fiber([]() {
        try {
            std::cout << "[Fiber] Testing with Agent..." << std::endl;
            
            // Note: agent is static, can be accessed directly
            std::string test_ws = "./test_real_ws";
            fs::remove_all(test_ws);
            fs::create_directories(test_ws);
            
            // Note: Agent internally uses the workspace from Config, 
            // but for this test we'll manually ensure directories exist
            // and trigger distillation on a dummy session.
            
            Session session;
            session.key = "real_test_session";
            session.add_message("user", "What are the core design principles of the OpenClaw memory system?");
            session.add_message("assistant", "OpenClaw uses a three-tier memory architecture: Raw sessions, Daily Logs, and Curated Memory.");
            session.add_message("user", "Explain hybrid search.");
            session.add_message("assistant", "It combines BM25 keyword search with Faiss vector search.");

            std::cout << "[Fiber] Triggering L1 -> L2 Distillation (Real Call)..." << std::endl;
            // This will use the real LLM configured in config.yaml
            agent.loop().distill_l1_to_l2(session, 0, (int)session.messages.size(), AgentLoop::DistillationEvent::SESSION_END);
            
            std::cout << "[Fiber] Triggering L2 -> L3 Consolidation (Real Call)..." << std::endl;
            agent.loop().consolidate_memory(session, 0, (int)session.messages.size());
            
            std::cout << "[Fiber] Verification..." << std::endl;
            // Check if any log files were created in the actual workspace
            // Note: AgentLoop uses workspace from Config::instance().memory_workspace()
            std::string real_ws = Config::instance().memory_workspace();
            std::cout << "Checking workspace: " << real_ws << std::endl;
            
            bool found_log = false;
            for (const auto& entry : fs::directory_iterator(fs::path(real_ws) / "memory")) {
                if (entry.path().extension() == ".md") {
                    std::cout << "Found memory file: " << entry.path().filename() << std::endl;
                    found_log = true;
                }
            }
            
            if (found_log) {
                std::cout << "\n✅ Real Distillation Test Successful (Check logs for API response details)" << std::endl;
            } else {
                std::cout << "\n❌ No memory files created. Check if API calls failed." << std::endl;
            }

        } catch (const std::exception& e) {
            std::cerr << "Exception in test fiber: " << e.what() << std::endl;
        }
        
        test_finished = true;
    });

    // Run the event loop (init_spawn_system and spawn_in_fiber handle initialization)
    // We just need to wait for the fiber to finish or timeout
    int waits = 0;
    while (!test_finished && waits < 600) { // 60s timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waits++;
    }

    if (test_finished) {
        std::cout << "Exiting cleanly." << std::endl;
        exit(0);
    } else {
        std::cout << "Test timed out after 60s" << std::endl;
        exit(1);
    }
}
