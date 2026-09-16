#pragma once
// ContextBuilder — assembles the system prompt from bootstrap files, memory, skills.
// Native function-calling version: no Interaction Format in system prompt.

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>

#include "memory.hpp"
#include "skills.hpp"
#include "agent_types.hpp"
#include <functional>

#include <simdjson.h>

class ContextBuilder {
public:
    static constexpr const char* BOOTSTRAP_FILES[] = {
        "AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"
    };

    explicit ContextBuilder(const std::string& workspace, EmbeddingFn embed_fn = nullptr)
        : workspace_(workspace)
        , memory_(workspace, std::move(embed_fn))
        , skills_(workspace)
    {}

    std::string build_system_prompt(const std::string& channel = "", const std::string& chat_id = "") const {
        std::vector<std::string> parts;

        parts.push_back(get_identity());

        std::string bootstrap = load_bootstrap_files();
        if (!bootstrap.empty()) parts.push_back(bootstrap);

        std::string mem = memory_.get_memory_context();
        if (!mem.empty()) parts.push_back("# Memory\n\n" + mem);

        std::string always = skills_.load_always_skills();
        if (!always.empty()) parts.push_back("# Active Skills\n\n" + always);

        std::string summary = skills_.build_skills_summary();
        if (!summary.empty()) {
            parts.push_back(
                "# Available Skills\n\n"
                "The following skills extend your capabilities. "
                "To use a skill, read its SKILL.md file with `read_file`.\n\n"
                + summary
            );
        }

        std::string prompt = join(parts, "\n\n---\n\n");

        // Topic context: the mobile UIs (iOS/Android) write <workspace>/topics.json
        // describing their Slack-style topics. When the current session matches
        // one, tell the model what this channel is for — e.g. #meetings is a work
        // journal where it should analyze behavior patterns and suggest changes.
        std::string topic = load_topic_context(chat_id);
        if (!topic.empty()) {
            prompt += "\n\n## Current Topic\n" + topic;
        } else if (!channel.empty() && !chat_id.empty()) {
            prompt += "\n\n## Current Session\nChannel: " + channel + "\nChat ID: " + chat_id;
        }

        return prompt;
    }

    std::vector<Message> build_messages(
        const std::vector<Message>& history,
        const std::string& current_message,
        const std::string& channel = "",
        const std::string& chat_id = ""
    ) const {
        std::vector<Message> msgs;
        std::string sys = build_system_prompt(channel, chat_id);
        spdlog::debug("System Prompt length: {} chars", sys.size());
        msgs.push_back({"system", sys, "", "", ""});
        for (const auto& h : history) msgs.push_back(h);
        msgs.push_back({"user", current_message, "", "", ""});
        return msgs;
    }

    MemoryStore& memory() { return memory_; }
    const MemoryStore& memory() const { return memory_; }

private:
    std::string workspace_;
    MemoryStore memory_;
    SkillsLoader skills_;

    std::string get_identity() const {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&t);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M (%A)", &tm);

        std::string ws = fs::absolute(fs::path(workspace_)).string();

        std::ostringstream ss;
        ss << "# miniclaw 🦞\n\n"
           << "You are miniclaw, a high-performance personal AI assistant.\n\n"
           << "## Current Time\n" << time_buf << "\n\n"
           << "## Workspace\n"
           << "Your workspace is at: " << ws << "\n"
           << "- Long-term memory: memory/MEMORY.md\n"
           << "- History log: memory/HISTORY.md\n"
           << "- Skills: skills/\n\n"
           << "Always be helpful, accurate, and concise.\n"
           << "When remembering something important, write to memory/MEMORY.md\n"
           << "To recall past events, use exec to grep memory/HISTORY.md";
        return ss.str();
    }

    std::string load_bootstrap_files() const {
        std::vector<std::string> parts;
        for (const char* fname : BOOTSTRAP_FILES) {
            fs::path p = fs::path(workspace_) / fname;
            if (!fs::exists(p)) continue;
            std::ifstream f(p);
            if (!f.is_open()) continue;
            std::ostringstream ss;
            ss << f.rdbuf();
            parts.push_back("## " + std::string(fname) + "\n\n" + ss.str());
        }
        return join(parts, "\n\n");
    }

    /// Reads <workspace>/topics.json (written by the mobile app on every
    /// change) and returns a system-prompt section describing the topic whose
    /// id matches `session_id` — or "" when the file is missing/unreadable or
    /// the session is not a known topic. Format: [{"id","name","blurb","purpose"}]
    std::string load_topic_context(const std::string& session_id) const {
        if (session_id.empty()) return "";
        fs::path p = fs::path(workspace_) / "topics.json";
        if (!fs::exists(p)) return "";
        std::ifstream f(p);
        if (!f.is_open()) return "";
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();

        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (parser.parse(text).get(root)) return "";
        if (!root.is_array()) return "";
        for (auto t : root) {
            std::string_view id_sv, name_sv, blurb_sv, purpose_sv;
            if (t["id"].get(id_sv)) continue;
            if (std::string(id_sv) != session_id) continue;
            std::string out = "You are conversing in the \"#" +
                              (t["name"].get(name_sv) ? std::string(id_sv) : std::string(name_sv)) +
                              "\" topic.";
            if (!t["blurb"].get(blurb_sv) && !blurb_sv.empty())
                out += " Purpose: " + std::string(blurb_sv) + ".";
            if (!t["purpose"].get(purpose_sv) && !purpose_sv.empty())
                out += "\n" + std::string(purpose_sv);
            return out;
        }
        return "";
    }

    static std::string join(const std::vector<std::string>& v, const std::string& sep) {
        std::ostringstream ss;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) ss << sep;
            ss << v[i];
        }
        return ss.str();
    }
};
