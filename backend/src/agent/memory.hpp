#pragma once
// MemoryStore — mirrors nanobot/nanobot/agent/memory.py
// Two-layer memory:
//   - Long-term facts:  workspace/memory/MEMORY.md   (curated, written by agent)
//   - History log:      workspace/memory/HISTORY.md  (append-only, grep-searchable)

#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

class MemoryStore {
public:
    explicit MemoryStore(const std::string& workspace)
        : workspace_(workspace)
        , memory_dir_(fs::path(workspace) / "memory")
        , memory_file_(memory_dir_ / "MEMORY.md")
        , history_file_(memory_dir_ / "HISTORY.md")
    {
        fs::create_directories(memory_dir_);
    }

    // ── Long-term memory (MEMORY.md) ─────────────────────────────────────────

    std::string read_long_term() const {
        return read_file(memory_file_);
    }

    void write_long_term(const std::string& content) {
        write_file(memory_file_, content);
    }

    // ── History log (HISTORY.md) ─────────────────────────────────────────────

    void append_history(const std::string& entry) {
        std::ofstream f(history_file_, std::ios::app);
        if (f.is_open()) {
            f << entry << "\n\n";
        }
    }

    // ── Context for system prompt ─────────────────────────────────────────────

    std::string get_memory_context() const {
        std::string lt = read_long_term();
        if (lt.empty()) return "";
        return "## Long-term Memory\n\n" + lt;
    }

private:
    fs::path workspace_;
    fs::path memory_dir_;
    fs::path memory_file_;
    fs::path history_file_;

    static std::string read_file(const fs::path& p) {
        if (!fs::exists(p)) return "";
        std::ifstream f(p);
        if (!f.is_open()) return "";
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static void write_file(const fs::path& p, const std::string& content) {
        std::ofstream f(p);
        if (f.is_open()) f << content;
    }
};
