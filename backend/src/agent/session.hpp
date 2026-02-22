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
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include "../json_util.hpp"

#include "context.hpp" // For Message struct

namespace fs = std::filesystem;

struct Session {
    std::string key;
    std::vector<Message> messages;
    std::string created_at;
    std::string updated_at;
    std::string metadata = "{}"; // Store raw JSON string
    int last_consolidated = 0;
    std::string last_consolidation_date;

    void add_message(const std::string& role, const std::string& content) {
        messages.push_back({role, content});
        updated_at = current_iso_timestamp();
    }

    size_t estimate_tokens() const {
        size_t tokens = 0;
        for (const auto& msg : messages) {
            tokens += (msg.role.length() + msg.content.length()) / 4 + 1;
        }
        return tokens;
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
        std::string meta_line = "{\"_type\":\"metadata\",\"created_at\":\"" + session.created_at + 
                               "\",\"updated_at\":\"" + session.updated_at + 
                               "\",\"metadata\":" + session.metadata + 
                               ",\"last_consolidated\":" + std::to_string(session.last_consolidated) + 
                               ",\"last_consolidation_date\":\"" + session.last_consolidation_date + "\"}";
        f << meta_line << "\n";

        // Message lines
        for (const auto& msg : session.messages) {
            std::string jmsg = "{\"role\":\"" + json_util::escape(msg.role) + 
                              "\",\"content\":\"" + json_util::escape(msg.content) + "\"}";
            f << jmsg << "\n";
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
        simdjson::dom::parser parser;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            simdjson::dom::element data;
            auto error = parser.parse(line).get(data);
            if (!error) {
                std::string_view type_sv;
                if (!data["_type"].get(type_sv) && type_sv == "metadata") {
                    // It's metadata
                    session.metadata = simdjson::to_string(data["metadata"]);
                    
                    std::string_view created_sv;
                    (void)data["created_at"].get(created_sv);
                    session.created_at = std::string(created_sv);
                    
                    int64_t consolidated;
                    if (!data["last_consolidated"].get(consolidated)) {
                        session.last_consolidated = (int)consolidated;
                    }
                    std::string_view date_sv;
                    if (!data["last_consolidation_date"].get(date_sv)) {
                        session.last_consolidation_date = std::string(date_sv);
                    }
                } else {
                    std::string_view role_sv, content_sv;
                    (void)data["role"].get(role_sv);
                    (void)data["content"].get(content_sv);
                    session.messages.push_back({std::string(role_sv), std::string(content_sv)});
                }
            } else {
                spdlog::warn("simdjson parse error in session load: {} (line: {})", (int)error, line);
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
