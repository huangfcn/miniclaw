#pragma once
#include <yaml-cpp/yaml.h>
#include <string>
#include <iostream>
#include <filesystem>

class Config {
public:
    static Config& instance() {
        static Config inst;
        return inst;
    }

    void load(const std::string& path = "config.yaml") {
        std::string actual_path = path;
        if (!std::filesystem::exists(actual_path)) {
            // Try parent directory
            std::filesystem::path p = std::filesystem::path("..") / path;
            if (std::filesystem::exists(p)) {
                actual_path = p.string();
            } else {
                std::cerr << "Config file not found: " << path << " or " << p << ". Using defaults." << std::endl;
                return;
            }
        }
        config_ = YAML::LoadFile(actual_path);
    }

    // Server
    int server_port() const { return get("server", "port", 9000); }
    int server_threads() const { return get("server", "threads", 4); }

    // OpenAI
    std::string openai_model() const { return get<std::string>("openai", "model", "gpt-4o-mini"); }
    std::string openai_api_key() const { 
        std::string key = get<std::string>("openai", "api_key", "");
        if (key.empty()) {
            const char* env_key = std::getenv("OPENAI_API_KEY");
            if (env_key) key = env_key;
        }
        return key;
    }

    // Memory
    std::string memory_workspace() const { 
        std::filesystem::path ws = ".";
        const char* env_ws = std::getenv("WORKSPACE_DIR");
        if (env_ws) {
            ws = env_ws;
        } else if (config_["memory"] && config_["memory"]["workspace"]) {
            ws = config_["memory"]["workspace"].as<std::string>();
        }
        return std::filesystem::absolute(ws).lexically_normal().string();
    }
    int memory_consolidation_threshold() const { return get("memory", "consolidation_threshold", 100); }

    // Logging
    std::string logging_level() const { return get<std::string>("logging", "level", "info"); }
    std::string logging_file() const { return get<std::string>("logging", "file", "backend.log"); }
    bool logging_enabled() const { return get<bool>("logging", "enabled", true); }

    // Skills
    std::string skills_path() const { return get<std::string>("skills", "path", "skills"); }

private:
    Config() = default;
    YAML::Node config_;

    template<typename T>
    T get(const std::string& section, const std::string& key, T default_val) const {
        if (config_[section] && config_[section][key]) {
            return config_[section][key].as<T>();
        }
        return default_val;
    }
};
