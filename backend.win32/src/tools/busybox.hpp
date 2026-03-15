#pragma once
#include "tool.hpp"
#include "../config.hpp"
#include <array>
#include <memory>
#include <cstdio>
#include <map>
#include <filesystem>
#include <spdlog/spdlog.h>

class BusyBoxTool : public Tool {
public:
    std::string name() const override { return "bash"; }
    std::string description() const override {
        return "Execute a Bash command on Windows using BusyBox. Use for ls, grep, find, cat, etc.";
    }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"bash","description":"Execute a Bash command on Windows using BusyBox. Use for ls, grep, find, cat, etc.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"The command to execute (e.g., 'ls -la', 'grep foo bar.txt')"}},"required":["command"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it = args.find("command");
        if (it == args.end()) return "Error: missing 'command' argument";
        return execute(it->second);
    }

    std::string execute(const std::string& input) override {
#if defined(_WIN32)
        spdlog::info("BusyBoxTool: executing command: {}", input);
        std::string tools_dir = Config::instance().tools_path();
        std::filesystem::path bb_path = std::filesystem::path(tools_dir) / "busybox.exe";
        
        // Fallback for development if not in tools_dir yet
        if (!std::filesystem::exists(bb_path)) {
            bb_path = std::filesystem::absolute("tools/bin/busybox.exe");
        }

        if (!std::filesystem::exists(bb_path)) {
            spdlog::error("BusyBoxTool: busybox.exe not found at {}", bb_path.string());
            return "Error: busybox.exe not found at " + bb_path.string();
        }

        std::string full_command = "\"" + bb_path.string() + "\" " + input;
        
        std::array<char, 4096> buffer;
        std::string result;
        
        // Use _popen to run the command on Windows
        FILE* pipe = _popen(full_command.c_str(), "r");

        if (!pipe) {
            spdlog::error("BusyBoxTool: _popen() failed for command: {}", input);
            return "Error: _popen() failed!";
        }

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }

        int exit_code = _pclose(pipe);
        spdlog::info("BusyBoxTool: command finished with exit code {}", exit_code);

        if (result.empty()) result = "(no output)";
        spdlog::debug("BusyBoxTool: command output (first 100 chars): {}", result.substr(0, 100));
        return result;
#else
        spdlog::warn("BusyBoxTool: 'bash' tool called on non-Windows platform");
        return "Error: The 'bash' tool (via BusyBox) is only available on Windows. On Linux/macOS, use the 'exec' tool for native shell commands.";
#endif
    }
};
