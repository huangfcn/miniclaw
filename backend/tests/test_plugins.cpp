// Plugin system smoke test:
// 1. Load the travel plugin (plugin.json + plugin.js) from the workspace.
// 2. Verify the tool is registered with the right name/description/schema.
// 3. Execute the travel handler (no Amadeus keys -> graceful fallback path).
// 4. Execute rerank against the saved last search.

#include "agent.hpp"
#include "agent/fiber_pool.hpp"
#include "config.hpp"
#include "plugins/js_plugin.hpp"
#include <fiber.hpp>
#include <fiber.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

static std::atomic<int> failures{0};

#ifdef MC_PLUGIN_TEST_SRC
#define PLUGIN_SRC MC_PLUGIN_TEST_SRC
#endif

static void ensure_travel_plugin(const fs::path &workspace) {
    fs::path dst = fs::path(workspace) / "plugins" / "travel";
    if (fs::exists(dst / "plugin.json")) return;
#ifdef PLUGIN_SRC
    fs::path src(PLUGIN_SRC "/travel");
    if (fs::exists(src / "plugin.json")) {
        fs::create_directories(dst);
        fs::copy(src / "plugin.json", dst / "plugin.json",
                 fs::copy_options::overwrite_existing);
        fs::copy(src / "plugin.js", dst / "plugin.js",
                 fs::copy_options::overwrite_existing);
        std::cout << "[Test] copied travel plugin from " << src << std::endl;
        return;
    }
#endif
    std::cerr << "[Test] no travel plugin found; skipping" << std::endl;
    std::exit(2);
}

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting JS plugin system test");

    Config::instance().load();
    std::string workspace = Config::instance().memory_workspace();
    std::cout << "[Test] workspace: " << workspace << std::endl;
    ensure_travel_plugin(workspace);

    curl_global_init(CURL_GLOBAL_ALL);
    FiberGlobalStartup();
    static Agent global_agent;
    FiberPool::instance().init(1, &global_agent);

    static std::atomic<bool> done{false};
    FiberPool::instance().spawn([&]() {
        fiber_create([](void *arg) -> void * {
            std::atomic<bool> *d = (std::atomic<bool> *)arg;
            try {
                plugins::PluginLoader loader(Config::instance().memory_workspace());
                auto tools = loader.load();

                std::cout << "[Test] loaded " << tools.size() << " plugin tool(s)"
                          << std::endl;
                auto travel =
                    std::find_if(tools.begin(), tools.end(), [](auto &t) {
                        return t->name() == "travel";
                    });
                if (travel == tools.end()) {
                    std::cerr << "[FAIL] travel tool not loaded" << std::endl;
                    failures++;
                } else {
                    std::cout << "[Test] tool name: " << (*travel)->name()
                              << std::endl;
                    std::cout << "[Test] tool description: "
                              << (*travel)->description() << std::endl;
                    std::cout << "[Test] schema: " << (*travel)->schema()
                              << std::endl;
                    if ((*travel)->schema().find("\"travel\"") == std::string::npos) {
                        std::cerr << "[FAIL] schema missing tool name" << std::endl;
                        failures++;
                    }

                    // Execute: search without Amadeus keys -> must not throw and
                    // must report the missing-keys state (graceful fallback).
                    std::string res = (*travel)->execute({
                        {"action", "search"},
                        {"origin", "Vienna"},
                        {"destination", "Paris"},
                        {"date", "2026-10-10"},
                        {"transport", "flights"},
                    });
                    std::cout << "[Test] search result:\n" << res << std::endl;
                    if (res.find("No live offers") == std::string::npos &&
                        res.find("Amadeus") == std::string::npos) {
                        std::cerr << "[FAIL] search result missing fallback message"
                                  << std::endl;
                        failures++;
                    }

                    // Execute: rerank (no meaningful offers yet -> must not throw).
                    std::string res2 =
                        (*travel)->execute({{"action", "rerank"}, {"sort", "price"}});
                    std::cout << "[Test] rerank result:\n" << res2 << std::endl;
                }
            } catch (const std::exception &e) {
                std::cerr << "[FAIL] exception: " << e.what() << std::endl;
                failures++;
            }
            *d = true;
            return nullptr;
        },
                       &done, NULL, 1024 * 1024);
    });
    while (!done.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (failures.load() > 0) {
        std::cerr << "PLUGIN TEST FAILED (" << failures.load() << ")" << std::endl;
        return 1;
    }
    std::cout << "PLUGIN TEST PASSED" << std::endl;
    return 0;
}
