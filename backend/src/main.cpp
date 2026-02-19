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
#include "config.hpp"

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
    // Load Configuration
    Config::instance().load();

    // Configure multi-sink logger (Console + File)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    
    std::string level_str = Config::instance().logging_level();
    spdlog::level::level_enum log_level = spdlog::level::from_str(level_str);
    console_sink->set_level(log_level);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(console_sink);

    if (Config::instance().logging_enabled()) {
        std::string log_file = Config::instance().logging_file();
        std::string log_path = log_file;
        const char* env_ws = std::getenv("WORKSPACE_DIR");
        if (env_ws) {
            log_path = std::string(env_ws) + "/" + log_file;
        }

        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path, false);
        file_sink->set_level(log_level);
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_level(log_level);
    spdlog::flush_on(spdlog::level::debug);

    spdlog::info("Starting miniclaw Backend (C++) with uWebSockets");

    FiberGlobalStartup();
    
    static Agent global_agent;

    // Initialize FiberPool with threads and the global agent from config
    int threads = Config::instance().server_threads();
    FiberPool::instance().init(threads, &global_agent);
    
    init_spawn_system();

    int port = Config::instance().server_port();
    spdlog::info("Backend running on port {} (Non-blocking Fiber Nodes)", port);

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
