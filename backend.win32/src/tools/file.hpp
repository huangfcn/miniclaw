#pragma once
#include "tool.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <map>

namespace fs = std::filesystem;

class ReadFileTool : public Tool {
public:
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "Read the contents of a file."; }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"read_file","description":"Read the contents of a file.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Absolute or relative path to the file"}},"required":["path"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it = args.find("path");
        if (it == args.end()) return "Error: missing 'path' argument";
        return execute(it->second);
    }

    std::string execute(const std::string& input) override {
        if (!fs::exists(input)) return "Error: File not found: " + input;
        std::ifstream f(input);
        if (!f.is_open()) return "Error: Cannot open file: " + input;
        std::stringstream buffer;
        buffer << f.rdbuf();
        return buffer.str();
    }
};

class WriteFileTool : public Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "Write content to a file (creates parent directories if needed)."; }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"write_file","description":"Write content to a file (creates parent directories if needed).","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Path to write to"},"content":{"type":"string","description":"Content to write"}},"required":["path","content"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto path_it = args.find("path");
        auto content_it = args.find("content");
        if (path_it == args.end()) return "Error: missing 'path' argument";
        if (content_it == args.end()) return "Error: missing 'content' argument";

        fs::path path(path_it->second);
        if (path.has_parent_path()) fs::create_directories(path.parent_path());
        std::ofstream f(path_it->second);
        if (!f.is_open()) return "Error: Cannot write to file: " + path_it->second;
        f << content_it->second;
        return "File written successfully: " + path_it->second;
    }

    // Legacy: first line = path, rest = content
    std::string execute(const std::string& input) override {
        size_t newline_pos = input.find('\n');
        if (newline_pos == std::string::npos)
            return "Error: Invalid input format. Expected path\\ncontent";
        std::string path_str = input.substr(0, newline_pos);
        std::string content = input.substr(newline_pos + 1);
        std::map<std::string, std::string> args = {{"path", path_str}, {"content", content}};
        return execute(args);
    }
};

class EditFileTool : public Tool {
public:
    std::string name() const override { return "edit_file"; }
    std::string description() const override { return "Replace specific text in a file."; }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"edit_file","description":"Replace specific text in a file.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"old_text":{"type":"string","description":"Text to replace"},"new_text":{"type":"string","description":"Replacement text"}},"required":["path","old_text","new_text"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto path_it = args.find("path");
        auto old_it  = args.find("old_text");
        auto new_it  = args.find("new_text");
        if (path_it == args.end()) return "Error: missing 'path'";
        if (old_it  == args.end()) return "Error: missing 'old_text'";
        if (new_it  == args.end()) return "Error: missing 'new_text'";

        if (!fs::exists(path_it->second)) return "Error: File not found: " + path_it->second;
        std::ifstream fin(path_it->second);
        std::stringstream buf;
        buf << fin.rdbuf();
        std::string content = buf.str();

        size_t pos = content.find(old_it->second);
        if (pos == std::string::npos) return "Error: old_text not found in file";
        content.replace(pos, old_it->second.length(), new_it->second);

        std::ofstream fout(path_it->second);
        fout << content;
        return "File edited successfully: " + path_it->second;
    }

    std::string execute(const std::string& input) override {
        return "Error: edit_file requires named arguments (path, old_text, new_text)";
    }
};

class ListDirTool : public Tool {
public:
    std::string name() const override { return "list_dir"; }
    std::string description() const override { return "List the contents of a directory."; }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"list_dir","description":"List the contents of a directory.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Path to the directory"}},"required":["path"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it = args.find("path");
        if (it == args.end()) return "Error: missing 'path'";
        return execute(it->second);
    }

    std::string execute(const std::string& input) override {
        if (!fs::exists(input)) return "Error: Path not found: " + input;
        std::string result;
        for (const auto& entry : fs::directory_iterator(input)) {
            result += entry.path().filename().string();
            if (entry.is_directory()) result += "/";
            result += "\n";
        }
        return result.empty() ? "(empty directory)" : result;
    }
};
