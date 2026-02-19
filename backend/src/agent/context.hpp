#pragma once
// ContextBuilder — mirrors nanobot/nanobot/agent/context.py
// Assembles the system prompt from:
//   AGENTS.md, SOUL.md, USER.md, TOOLS.md, IDENTITY.md  (bootstrap files)
//   MemoryStore  (long-term memory)
//   SkillsLoader (skills summary + always-loaded skills)

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>

#include "memory.hpp"
#include "skills.hpp"

namespace fs = std::filesystem;

// Mirrors nanobot's Message dict
struct Message {
    std::string role;    // "system" | "user" | "assistant" | "tool"
    std::string content;
};

class ContextBuilder {
public:
    // Bootstrap files loaded in order (same list as nanobot's BOOTSTRAP_FILES)
    static constexpr const char* BOOTSTRAP_FILES[] = {
        "AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"
    };

    explicit ContextBuilder(const std::string& workspace)
        : workspace_(workspace)
        , memory_(workspace)
        , skills_(workspace)
    {}

    // Build the full system prompt
    std::string build_system_prompt() const {
        std::vector<std::string> parts;

        // 1. Core identity block (runtime info, workspace paths)
        parts.push_back(get_identity());

        // 2. Bootstrap markdown files
        std::string bootstrap = load_bootstrap_files();
        if (!bootstrap.empty()) parts.push_back(bootstrap);

        // 3. Long-term memory
        std::string mem = memory_.get_memory_context();
        if (!mem.empty()) parts.push_back("# Memory\n\n" + mem);

        // 4. Always-loaded skills (full content)
        std::string always = skills_.load_always_skills();
        if (!always.empty()) parts.push_back("# Active Skills\n\n" + always);

        // 5. Skills summary (agent reads full SKILL.md via read_file tool)
        std::string summary = skills_.build_skills_summary();
        if (!summary.empty()) {
            parts.push_back(
                "# Skills\n\n"
                "The following skills extend your capabilities. "
                "To use a skill, read its SKILL.md file using the read_file tool.\n\n"
                + summary
            );
        }

        return join(parts, "\n\n---\n\n");
    }

    // Build the full message list for an LLM call (system + history + current user message)
    std::vector<Message> build_messages(
        const std::vector<Message>& history,
        const std::string& current_message
    ) const {
        std::vector<Message> msgs;
        msgs.push_back({"system", build_system_prompt()});
        for (const auto& h : history) msgs.push_back(h);
        msgs.push_back({"user", current_message});
        return msgs;
    }

    // Accessors so AgentLoop can use memory/skills directly
    MemoryStore& memory() { return memory_; }
    const MemoryStore& memory() const { return memory_; }

private:
    std::string workspace_;
    MemoryStore memory_;
    SkillsLoader skills_;

    std::string get_identity() const {
        // Current time
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&t);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M (%A)", &tm);

        std::string ws = fs::absolute(fs::path(workspace_)).string();

        std::ostringstream ss;
        ss << "# miniclaw 🦞\n\n"
           << "You are miniclaw, a high-performance autonomous AI agent. "
           << "You operate in a ReAct loop: **Thought → Action → Observation**. "
           << "Always reason about your next step before using a tool.\n\n"
           << "## Interaction Format\n"
           << "1. **Thought**: Explain what you are doing and why.\n"
           << "2. **Action**: Call a tool using XML format:\n"
           << "   <tool name=\"tool_name\">input_args</tool>\n"
           << "3. **Observation**: You will receive the tool output. Use it to decide your next move.\n\n"
           << "## Available Tools\n"
           << "- **terminal**: Execute shell commands. Input: command string.\n"
           << "- **read_file**: Read file content. Input: absolute or relative path.\n"
           << "- **write_file**: Write content to file. Input: `<path>\\n<content>` (Path on 1st line, content on subsequent lines).\n"
           << "- **web_search**: Search the web. Input: query string.\n"
           << "- **web_fetch**: Fetch a URL. Input: URL string.\n"
           << "- **spawn**: Spawn a background subagent for a sub-task. Input: task description.\n\n"
           << "## Current Context\n"
           << "- Time: " << time_buf << "\n"
           << "- Workspace: " << ws << "\n"
           << "- Memory: " << ws << "/memory/MEMORY.md\n"
           << "- History: " << ws << "/memory/HISTORY.md\n\n"
           << "When you have a final answer, provide it clearly. If a task requires multiple steps, do them one by one.";
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

    static std::string join(const std::vector<std::string>& v, const std::string& sep) {
        std::ostringstream ss;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) ss << sep;
            ss << v[i];
        }
        return ss.str();
    }
};
