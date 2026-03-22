#pragma once
#include "tool.hpp"
#include <array>
#include <memory>
#include <cstdio>
#include <map>
#include "busybox.hpp"

class TerminalTool : public Tool {
public:
    std::string name() const override { return "exec"; }
    std::string description() const override {
        return "Execute a raw shell command literally. Provides Bash-like utilities (ls, grep, cat, etc.) via BusyBox on Windows. Do NOT add prefixes like 'shell:', 'bash:', or 'cmd /c' unless you specifically intend to run them.";
    }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"exec","description":"Execute a raw shell command literally. Provides Bash-like utilities (ls, grep, cat, etc.) via BusyBox on Windows. Do NOT add prefixes like 'shell:', 'bash:', or 'cmd /c' unless you specifically intend to run them.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"The raw, literal shell command string to execute (e.g. 'ls -la', 'grep keyword file.txt')"}},"required":["command"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it = args.find("command");
        if (it == args.end()) return "Error: missing 'command' argument";
        return execute(it->second);
    }

    std::string execute(const std::string& input) override {
#if defined(_WIN32)
        static BusyBoxTool bb;
        return bb.execute(input);
#else
        std::array<char, 4096> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(input.c_str(), "r"), pclose);
        if (!pipe) {
            return "Error: popen() failed!";
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        if (result.empty()) result = "(no output)";
        return result;
#endif
    }
};
