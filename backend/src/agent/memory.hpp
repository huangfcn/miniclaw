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

#include "memory_index.hpp"
#include "config.hpp"

class MemoryStore {
public:
    explicit MemoryStore(const std::string& workspace)
        : workspace_(workspace)
        , memory_dir_(fs::path(workspace) / "memory")
        , memory_file_(memory_dir_ / "MEMORY.md")
        , index_(std::make_unique<MemoryIndex>(fs::path(workspace) / "index", Config::instance().embedding_dimension()))
    {
        fs::create_directories(memory_dir_);
    }

    // ── Long-term memory (MEMORY.md - Layer 3) ───────────────────────────────

    std::string read_long_term() const {
        return read_file(memory_file_);
    }

    void write_long_term(const std::string& content) {
        write_file(memory_file_, content);
        // Index L3
        index_->add_document("L3_MEMORY", memory_file_.string(), 0, 0, content, {}, "long-term");
    }

    // ── Daily Logs (memory/YYYY-MM-DD.md - Layer 2) ─────────────────────────

    void append_daily_log(const std::string& content) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%d");
        fs::path daily_file = memory_dir_ / (ss.str() + ".md");

        std::ofstream f(daily_file, std::ios::app);
        if (f.is_open()) {
            f << content << "\n\n";
        }

        // Index the new entry (simplified: re-index full file or just the chunk)
        // For simplicity, we'll index this chunk. In a real system, we'd want more structure.
        std::string id = "L2_" + ss.str() + "_" + std::to_string(std::time(nullptr));
        index_->add_document(id, daily_file.string(), 0, 0, content, {}, "memory");
    }

    // ── Search ───────────────────────────────────────────────────────────────

    std::vector<SearchResult> search(const std::string& query, const std::vector<float>& embedding = {}) {
        return index_->search(query, embedding);
    }

    // ── Indexing Session (Layer 1) ──────────────────────────────────────────

    void index_session_message(const std::string& session_id, const std::string& role, const std::string& content) {
        std::string id = "L1_" + session_id + "_" + std::to_string(std::time(nullptr));
        index_->add_document(id, "session:" + session_id, 0, 0, "[" + role + "] " + content, {}, "sessions");
    }

    // ── Context for system prompt ─────────────────────────────────────────────

    std::string get_memory_context() const {
        std::stringstream ss;
        
        std::string lt = read_long_term();
        if (!lt.empty()) {
            ss << "## Long-term Memory (Curated Facts)\n\n" << lt << "\n\n";
        }

        auto yesterday = get_date_string(-1);
        auto today = get_date_string(0);

        std::string yesterday_log = read_file(memory_dir_ / (yesterday + ".md"));
        if (!yesterday_log.empty()) {
            ss << "## Daily Log (" << yesterday << ")\n\n" << yesterday_log << "\n\n";
        }

        std::string today_log = read_file(memory_dir_ / (today + ".md"));
        if (!today_log.empty()) {
            ss << "## Daily Log (" << today << " - Today)\n\n" << today_log << "\n\n";
        }

        return ss.str();
    }

    MemoryIndex& index() { return *index_; }

private:
    fs::path workspace_;
    fs::path memory_dir_;
    fs::path memory_file_;
    std::unique_ptr<MemoryIndex> index_;

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

    static std::string get_date_string(int offset_days) {
        auto now = std::chrono::system_clock::now();
        auto target = now + std::chrono::hours(24 * offset_days);
        auto in_time_t = std::chrono::system_clock::to_time_t(target);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%d");
        return ss.str();
    }
};
