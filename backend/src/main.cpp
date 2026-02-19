#include <nlohmann/json.hpp>
#include "agent.hpp"
#include <spdlog/spdlog.h>
#include <csignal> 
#include <curl/curl.h>
#include <fiber.h>
#include <uv.h>
#include <thread>
#include <condition_variable>
#include "agent/fiber_pool.hpp"

#include <atomic>
#include <unistd.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

// Globals for signal handling
static std::mutex mtx;
static std::condition_variable cv;
static bool exit_signal = false;

static std::atomic<time_t> last_ctrl_c_time{0};
void signal_handler(int signum) {
    time_t now = time(NULL);
    time_t last = last_ctrl_c_time.exchange(now);
    
    if (now - last <= 5 && last != 0) {
        const char msg[] = "\nReceived second Ctrl-C within 5s, forcing immediate exit!\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(signum);
    }
    
    const char msg[] = "\nReceived Ctrl-C, initiating graceful shutdown... (Press Ctrl-C again within 5s to force exit)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    
    {
        std::unique_lock<std::mutex> lock(mtx);
        exit_signal = true;
    }
    cv.notify_one();
}

int main() {
    // Configure multi-sink logger (Console + File)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::debug);

    std::string log_path = "backend.log";
    const char* ws_dir = std::getenv("WORKSPACE_DIR");
    if (ws_dir) {
        log_path = std::string(ws_dir) + "/backend.log";
    }

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path, false);
    file_sink->set_level(spdlog::level::debug);

    auto logger = std::make_shared<spdlog::logger>("multi_sink", spdlog::sinks_init_list{console_sink, file_sink});
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::info); // Ensure logs are flushed to file periodically

    spdlog::info("Starting miniclaw Backend (C++) with uWebSockets");

    FiberGlobalStartup();
    
    static Agent global_agent;

    // Initialize FiberPool with 4 threads and the global agent
    FiberPool::instance().init(4, &global_agent);
    
    init_spawn_system();

    spdlog::info("Backend running on port 9000 (Non-blocking Fiber Nodes)");

    // Register signal handler for Ctrl-C
    std::signal(SIGINT, signal_handler);

    // Keep the main thread alive
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&]{ return exit_signal; });

    spdlog::info("Initiating graceful shutdown...");
    FiberPool::instance().stop(); // Stop the FiberPool
    // FiberGlobalShutdown(); // Not defined in fiber.h
    curl_global_cleanup();        // Cleanup libcurl

    spdlog::info("Shutdown complete. Exiting.");
    return 0;
}
