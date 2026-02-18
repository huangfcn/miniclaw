#pragma once
#include "tool.hpp"
#include <array>
#include <memory>
#include <cstdio>

class TerminalTool : public Tool {
public:
    std::string name() const override { return "terminal"; }
    std::string description() const override { return "Execute shell commands"; }
    std::string execute(const std::string& input) override {
        // Simple popen implementation (Proof of Concept)
        // SECURITY ALERT: Proper escaping needed for production
        std::array<char, 128> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(input.c_str(), "r"), pclose);
        if (!pipe) {
            return "Error: popen() failed!";
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }
};
