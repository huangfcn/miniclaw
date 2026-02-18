#pragma once
#include "tool.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

class ReadFileTool : public Tool {
public:
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "Read file content. Input: file path"; }
    std::string execute(const std::string& input) override {
        // TODO: Sandbox check (allow only within workspace)
        if (!fs::exists(input)) {
            return "Error: File not found: " + input;
        }
        std::ifstream f(input);
        if (!f.is_open()) {
            return "Error: Cannot open file: " + input;
        }
        std::stringstream buffer;
        buffer << f.rdbuf();
        return buffer.str();
    }
};

class WriteFileTool : public Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "Write content to file. Input format: <path>\\n<content>"; }
    std::string execute(const std::string& input) override {
        // Naive parsing: first line is path, rest is content
        size_t newline_pos = input.find('\n');
        if (newline_pos == std::string::npos) {
            return "Error: Invalid input format. Expected path\\ncontent";
        }
        std::string path_str = input.substr(0, newline_pos);
        std::string content = input.substr(newline_pos + 1);
        
        // TODO: Sandbox check
        // Ensure directory exists
        fs::path path(path_str);
        if (path.has_parent_path()) {
            fs::create_directories(path.parent_path());
        }
        
        std::ofstream f(path_str);
        if (!f.is_open()) {
            return "Error: Cannot write to file: " + path_str;
        }
        f << content;
        return "File written successfully: " + path_str;
    }
};
