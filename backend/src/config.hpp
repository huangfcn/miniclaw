#pragma once
#include <yaml-cpp/yaml.h>
#include <string>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

class Config {
public:
    static Config& instance() {
        static Config inst;
        return inst;
    }

    void load(const std::string& path = "config/config.yaml") {
        std::string actual_path = path;
        if (!std::filesystem::exists(actual_path)) {
            // Try parent directory
            std::filesystem::path p = std::filesystem::path("..") / path;
            if (std::filesystem::exists(p)) {
                actual_path = p.string();
            } else {
                // Compatibility: try current directory if config/config.yaml not found
                if (std::filesystem::exists("config.yaml")) {
                    actual_path = "config.yaml";
                } else if (std::filesystem::exists("../config.yaml")) {
                    actual_path = "../config.yaml";
                } else {
                    std::cerr << "Config file not found: " << path << " or " << p << ". Using defaults." << std::endl;
                    return;
                }
            }
        }
        config_ = YAML::LoadFile(actual_path);
    }

    // Server
    int server_port() const { return get("server", "port", 9000); }
    int server_threads() const { return get("server", "threads", 4); }

    // Conversation LLM
    std::string conversation_provider() const { return get<std::string>("conversation", "provider", "openai"); }
    std::string conversation_model() const { return get<std::string>("conversation", "model", "gpt-4o-mini"); }
    std::string conversation_endpoint() const { return get<std::string>("conversation", "endpoint", "https://api.openai.com/v1/chat/completions"); }
    std::string conversation_api_key() const { 
        const char* s = std::getenv("OPENAI_API_KEY");
        if (s) return s;
        return get<std::string>("conversation", "api_key", "");
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
    int memory_l1_to_l2_threshold() const { return get("memory", "l1_to_l2_threshold", 30); }
    std::string memory_distillation_time() const { return get<std::string>("memory", "time", "13:00"); }
    int memory_context_window() const { return get("memory", "context_window", 128000); }
    float memory_compaction_threshold() const { return get("memory", "compaction_threshold", 0.8f); }
    std::string memory_distillation_provider() const { return get<std::string>("memory", "provider", "openai"); }
    std::string memory_distillation_model() const { return get<std::string>("memory", "model", conversation_model()); }
    std::string memory_distillation_endpoint() const { 
        std::string ep = get<std::string>("memory", "endpoint", "");
        if (ep.empty()) {
            // Fallback to OpenAI base URL or custom endpoint
            return "https://api.openai.com/v1/chat/completions";
        }
        return ep;
    }

    // Embedding
    std::string embedding_provider() const { return get<std::string>("embedding", "provider", "openai"); }
    std::string embedding_model() const { return get<std::string>("embedding", "model", "text-embedding-3-small"); }
    std::string embedding_endpoint() const { return get<std::string>("embedding", "endpoint", "https://api.openai.com/v1/embeddings"); }
    int embedding_dimension() const { return get<int>("embedding", "dimension", 1536); }

    // Logging
    std::string logging_level() const { return get<std::string>("logging", "level", "info"); }
    std::string logging_file() const { return get<std::string>("logging", "file", "backend.log"); }
    bool logging_enabled() const { return get<bool>("logging", "enabled", true); }

    // Skills
    std::string skills_path() const { return get<std::string>("skills", "path", "skills"); }

    // Prompts
    std::string load_prompt(const std::string& name, const std::string& default_prompt) const {
        fs::path p = fs::path("config/prompts") / (name + ".txt");
        if (!fs::exists(p)) {
            // Try parent
            p = fs::path("../config/prompts") / (name + ".txt");
        }
        
        if (fs::exists(p)) {
            std::ifstream f(p);
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                return ss.str();
            }
        }
        return default_prompt;
    }

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
