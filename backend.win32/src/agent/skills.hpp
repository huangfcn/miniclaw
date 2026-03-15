#pragma once
// SkillsLoader — mirrors nanobot/nanobot/agent/skills.py
// Skills are folders under workspace/skills/<name>/SKILL.md
// The loader scans them, parses YAML frontmatter, and builds an XML summary
// that is injected into the system prompt.

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

struct SkillInfo {
    std::string name;
    std::string path;       // absolute path to SKILL.md
    std::string description;
    bool always_load = false; // load full content into prompt
};

class SkillsLoader {
public:
    explicit SkillsLoader(const std::string& workspace)
        : workspace_(workspace)
        , skills_dir_(fs::path(workspace) / "skills")
    {}

    // List all skills found under workspace/skills/
    std::vector<SkillInfo> list_skills() const {
        std::vector<SkillInfo> result;
        if (!fs::exists(skills_dir_)) return result;

        for (const auto& entry : fs::directory_iterator(skills_dir_)) {
            if (!entry.is_directory()) continue;
            fs::path skill_file = entry.path() / "SKILL.md";
            if (!fs::exists(skill_file)) continue;

            SkillInfo info;
            info.name = entry.path().filename().string();
            info.path = skill_file.string();

            auto meta = parse_frontmatter(read_file(skill_file));
            info.description = meta.count("description") ? meta.at("description") : info.name;
            info.always_load  = meta.count("always") && (meta.at("always") == "true");
            result.push_back(info);
        }
        return result;
    }

    // Load full content of a single skill (strips frontmatter)
    std::string load_skill(const std::string& name) const {
        fs::path p = skills_dir_ / name / "SKILL.md";
        if (!fs::exists(p)) return "";
        std::string content = read_file(p);
        return strip_frontmatter(content);
    }

    // Build XML summary injected into system prompt (agent reads full SKILL.md via read_file tool)
    std::string build_skills_summary() const {
        auto skills = list_skills();
        if (skills.empty()) return "";

        std::ostringstream ss;
        ss << "<skills>\n";
        for (const auto& s : skills) {
            ss << "  <skill>\n";
            ss << "    <name>" << escape_xml(s.name) << "</name>\n";
            ss << "    <description>" << escape_xml(s.description) << "</description>\n";
            ss << "    <location>" << escape_xml(s.path) << "</location>\n";
            ss << "  </skill>\n";
        }
        ss << "</skills>";
        return ss.str();
    }

    // Load full content of skills marked always=true
    std::string load_always_skills() const {
        std::ostringstream ss;
        for (const auto& s : list_skills()) {
            if (!s.always_load) continue;
            std::string content = load_skill(s.name);
            if (!content.empty()) {
                ss << "### Skill: " << s.name << "\n\n" << content << "\n\n---\n\n";
            }
        }
        return ss.str();
    }

private:
    fs::path workspace_;
    fs::path skills_dir_;

    static std::string read_file(const fs::path& p) {
        std::ifstream f(p);
        if (!f.is_open()) return "";
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Parse simple YAML frontmatter (key: value lines between --- delimiters)
    static std::map<std::string, std::string> parse_frontmatter(const std::string& content) {
        std::map<std::string, std::string> meta;
        if (content.substr(0, 3) != "---") return meta;

        std::regex fm_re("^---\\n([\\s\\S]*?)\\n---");
        std::smatch m;
        if (!std::regex_search(content, m, fm_re)) return meta;

        std::istringstream ss(m[1].str());
        std::string line;
        while (std::getline(ss, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = trim(line.substr(0, colon));
            std::string val = trim(line.substr(colon + 1));
            // Strip surrounding quotes
            if (val.size() >= 2 && (val.front() == '"' || val.front() == '\''))
                val = val.substr(1, val.size() - 2);
            meta[key] = val;
        }
        return meta;
    }

    static std::string strip_frontmatter(const std::string& content) {
        if (content.substr(0, 3) != "---") return content;
        std::regex fm_re("^---\\n[\\s\\S]*?\\n---\\n");
        return std::regex_replace(content, fm_re, "");
    }

    static std::string escape_xml(const std::string& s) {
        std::string out;
        for (char c : s) {
            if      (c == '&')  out += "&amp;";
            else if (c == '<')  out += "&lt;";
            else if (c == '>')  out += "&gt;";
            else                out += c;
        }
        return out;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
};
