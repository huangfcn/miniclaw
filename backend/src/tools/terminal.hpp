#pragma once
#include "tool.hpp"
#include <array>
#include <memory>
#include <cstdio>
#include <map>

class TerminalTool : public Tool {
public:
    std::string name() const override { return "exec"; }
    std::string description() const override {
        return "Execute a shell command and return its output. Use for running scripts, curl, grep, etc.";
    }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"exec","description":"Execute a shell command and return its output. Use for running scripts, curl, grep, etc.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"The shell command to execute"}},"required":["command"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it = args.find("command");
        if (it == args.end()) return "Error: missing 'command' argument";
        return execute(it->second);
    }

    std::string execute(const std::string& input) override {
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
    }
};
