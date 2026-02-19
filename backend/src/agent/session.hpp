#pragma once
// SessionManager — mirrors nanobot/nanobot/session/manager.py
// Stores conversation history in JSONL format for persistence.

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "context.hpp" // For Message struct

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Session {
    std::string key;
    std::vector<Message> messages;
    std::string created_at;
    std::string updated_at;
    json metadata;
    int last_consolidated = 0;

    void add_message(const std::string& role, const std::string& content) {
        messages.push_back({role, content});
        updated_at = current_iso_timestamp();
    }

private:
    static std::string current_iso_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
        return ss.str();
    }
};

class SessionManager {
public:
    explicit SessionManager(const std::string& workspace)
        : workspace_(workspace) {
        // sessions_dir_ = fs::path(workspace) / "sessions";
        // Mirrors nanobot's ~/.nanobot/sessions but using ~/.miniclaw/sessions
        const char* home = std::getenv("HOME");
        if (home) {
            sessions_dir_ = fs::path(home) / ".miniclaw" / "sessions";
        } else {
            sessions_dir_ = fs::path(workspace) / "sessions";
        }
        fs::create_directories(sessions_dir_);
    }

    Session get_or_create(const std::string& key) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        if (cache_.count(key)) {
            return cache_[key];
        }

        Session session = load(key);
        session.key = key; // Ensure key is set
        cache_[key] = session;
        return session;
    }

    void save(const Session& session) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        fs::path path = get_session_path(session.key);
        std::ofstream f(path);
        if (!f.is_open()) {
            spdlog::error("Failed to open session file for writing: {}", path.string());
            return;
        }

        // Metadata line
        json meta = {
            {"_type", "metadata"},
            {"created_at", session.created_at},
            {"updated_at", session.updated_at},
            {"metadata", session.metadata},
            {"last_consolidated", session.last_consolidated}
        };
        f << meta.dump() << "\n";

        // Message lines
        for (const auto& msg : session.messages) {
            json jmsg = {{"role", msg.role}, {"content", msg.content}};
            f << jmsg.dump() << "\n";
        }

        cache_[session.key] = session;
    }

private:
    fs::path workspace_;
    fs::path sessions_dir_;
    std::map<std::string, Session> cache_;
    std::recursive_mutex mtx_;

    fs::path get_session_path(const std::string& key) {
        std::string safe_key = key;
        std::replace(safe_key.begin(), safe_key.end(), ':', '_');
        std::replace(safe_key.begin(), safe_key.end(), '/', '_');
        return sessions_dir_ / (safe_key + ".jsonl");
    }

    Session load(const std::string& key) {
        fs::path path = get_session_path(key);
        Session session;
        session.key = key;

        if (!fs::exists(path)) {
            session.created_at = current_iso_timestamp();
            session.updated_at = session.created_at;
            return session;
        }

        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            try {
                json data = json::parse(line);
                if (data.contains("_type") && data["_type"] == "metadata") {
                    session.metadata = data["metadata"];
                    session.created_at = data["created_at"];
                    session.last_consolidated = data["last_consolidated"];
                } else {
                    session.messages.push_back({data["role"], data["content"]});
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to parse session line: {}", e.what());
            }
        }
        return session;
    }

    static std::string current_iso_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
        return ss.str();
    }
};
