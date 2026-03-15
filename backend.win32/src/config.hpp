#pragma once
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

class Config {
public:
  static Config &instance() {
    static Config inst;
    return inst;
  }

  void load(const std::string &path = "config/config.yaml") {
    fs::path p = path;
    bool found = false;

    // 1. Check relative to CWD or EXE (Dev environment)
    if (fs::exists(p)) {
      found = true;
    } else {
      fs::path exe_dir = get_executable_dir();
      if (fs::exists(exe_dir / path)) {
        p = exe_dir / path;
        found = true;
      } else if (fs::exists(fs::path("..") / path)) {
        p = fs::path("..") / path;
        found = true;
      }
    }

    // 2. Check system config directory
    if (!found) {
      fs::path system_config = fs::path(get_default_workspace()) / path;
      if (fs::exists(system_config)) {
        p = system_config;
        found = true;
      }
    }

    // 3. Try to bootstrap from template if not found
    if (!found) {
      ensure_config_exists();
      fs::path system_config =
          fs::path(get_default_workspace()) / "config.yaml";
      if (fs::exists(system_config)) {
        p = system_config;
        found = true;
      }
    }

    if (!found) {
      std::cerr << "Config file not found: " << path << ". Using defaults."
                << std::endl;
      return;
    }

    config_ = YAML::LoadFile(p.string());
    actual_config_path_ = fs::absolute(p).string();
  }

  std::string config_file_path() const { return actual_config_path_; }

  // Server
  int server_port() const { return get("server", "port", 9000); }
  int server_threads() const { return get("server", "threads", 4); }

  // Conversation LLM
  std::string conversation_provider() const {
    return get<std::string>("conversation", "provider", "openai");
  }
  std::string conversation_model() const {
    return get<std::string>("conversation", "model", "gpt-4o-mini");
  }
  std::string conversation_endpoint() const {
    return get<std::string>("conversation", "endpoint",
                            "https://api.openai.com/v1/chat/completions");
  }
  std::string conversation_api_key() const {
    const char *s = std::getenv("OPENAI_API_KEY");
    if (s)
      return s;
    return get<std::string>("conversation", "api_key", "XXXXXXXX");
  }

  // Memory
  std::string memory_workspace() const {
    const char *env_ws = std::getenv("WORKSPACE_DIR");
    if (env_ws) {
      return fs::absolute(env_ws).lexically_normal().string();
    }

    if (config_["memory"] && config_["memory"]["workspace"]) {
      std::string ws = config_["memory"]["workspace"].as<std::string>();
      if (ws != "." && ws != "./") {
        fs::path p(ws);
        if (p.is_relative()) {
          fs::path config_dir = fs::path(actual_config_path_).parent_path();
          if (config_dir.empty())
            config_dir = ".";
          return fs::absolute(config_dir / p).lexically_normal().string();
        }
        return fs::absolute(p).lexically_normal().string();
      }
    }

    return fs::absolute(get_default_workspace()).lexically_normal().string();
  }

  static std::string get_default_workspace() {
#if defined(_WIN32)
    const char *home = std::getenv("USERPROFILE");
    if (!home)
      home = std::getenv("HOME");
#else
    const char *home = std::getenv("HOME");
#endif
    if (home) {
      return (fs::path(home) / ".miniclaw").string();
    }
    return ".miniclaw";
  }

  static fs::path get_executable_dir() {
#if defined(_WIN32)
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return fs::path(path).parent_path();
#elif defined(__APPLE__)
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0)
      return fs::path(path).parent_path();
#else
    char path[1024];
    ssize_t count = readlink("/proc/self/exe", path, sizeof(path));
    if (count != -1)
      return fs::path(std::string(path, count)).parent_path();
#endif
    return ".";
  }

  int memory_l1_to_l2_threshold() const {
    return get("memory", "l1_to_l2_threshold", 30);
  }
  std::string memory_l1_distillation_trigger() const {
    return get<std::string>("memory", "l1_distillation_trigger",
                            "message_count");
  }
  int memory_l1_token_threshold() const {
    return get("memory", "l1_token_threshold", 10000);
  }
  std::string memory_distillation_time() const {
    return get<std::string>("memory", "time", "13:00");
  }
  int memory_context_window() const {
    return get("memory", "context_window", 128000);
  }
  float memory_compaction_threshold() const {
    return get("memory", "compaction_threshold", 0.8f);
  }
  std::string memory_distillation_provider() const {
    return get<std::string>("memory", "provider", "openai");
  }
  std::string memory_distillation_model() const {
    return get<std::string>("memory", "model", conversation_model());
  }
  std::string memory_distillation_endpoint() const {
    std::string ep = get<std::string>("memory", "endpoint", "");
    if (ep.empty()) {
      // Fallback to OpenAI base URL or custom endpoint
      return "https://api.openai.com/v1/chat/completions";
    }
    return ep;
  }

  // Embedding
  std::string embedding_provider() const {
    return get<std::string>("embedding", "provider", "openai");
  }
  std::string embedding_model() const {
    return get<std::string>("embedding", "model", "text-embedding-3-small");
  }
  std::string embedding_endpoint() const {
    return get<std::string>("embedding", "endpoint",
                            "https://api.openai.com/v1/embeddings");
  }
  int embedding_dimension() const {
    return get<int>("embedding", "dimension", 1536);
  }

  // Logging
  std::string logging_level() const {
    return get<std::string>("logging", "level", "info");
  }
  std::string logging_file() const {
    return get<std::string>("logging", "file", "backend.log");
  }
  bool logging_enabled() const { return get<bool>("logging", "enabled", true); }

  // Skills
  std::string skills_path() const {
    std::string p = get<std::string>("skills", "path", "skills");
    fs::path path(p);
    if (path.is_relative()) {
      // Resolve skills path relative to the workspace
      return (fs::path(memory_workspace()) / path).string();
    }
    return p;
  }

  // Tools
  std::string tools_path() const {
    std::string default_tools = (fs::path(get_default_workspace()) / "tools").string();
    return get<std::string>("server", "tools", default_tools);
  }

  // Prompts
  std::string load_prompt(const std::string &name,
                          const std::string &default_prompt) const {
    // Priority:
    // 1. Workspace prompts (~/.miniclaw/prompts)
    // 2. Relative to config/prompts
    // 3. Fallbacks

    fs::path ws_prompts = fs::path(memory_workspace()) / "prompts";
    std::vector<fs::path> candidates = {
        ws_prompts / (name + ".txt"),
        ws_prompts / (name + ".md"),
        fs::path("config/prompts") / (name + ".txt"),
        fs::path("config/prompts") / (name + ".md"),
    };

    // Add relative to exe candidates
    fs::path exe_dir = get_executable_dir();
    candidates.push_back(exe_dir / "config/prompts" / (name + ".txt"));
    candidates.push_back(fs::path("..") / "config/prompts" / (name + ".txt"));

    fs::path p;
    for (const auto &c : candidates) {
      if (fs::exists(c)) {
        p = c;
        break;
      }
    }

    if (!p.empty() && fs::exists(p)) {
      spdlog::info("Loading prompt file: {}", p.string());
      std::ifstream f(p);
      if (f.is_open()) {
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
      }
    }
    return default_prompt;
  }

  void ensure_config_exists() {
    fs::path target = fs::path(get_default_workspace()) / "config/config.yaml";
    if (fs::exists(target))
      return;

    bootstrap_workspace();

    // Try to find a template
    fs::path template_path;
    fs::path exe_dir = get_executable_dir();

    std::vector<fs::path> candidates = {
        exe_dir / "../config/config.yaml",
        "./config/config.yaml"};

    // Add Tauri resource path if provided
    const char *res_env = std::getenv("RESOURCES_DIR");
    if (res_env) {
      fs::path res_path = fs::path(res_env) / "resources/workspace";
      candidates.push_back(res_path / "config/config.yaml");
    }

    for (const auto &c : candidates) {
      if (fs::exists(c)) {
        template_path = c;
        break;
      }
    }

    if (!template_path.empty()) {
      try {
        fs::copy_file(template_path, target,
                      fs::copy_options::overwrite_existing);
        std::cout << "Initial config copied from " << template_path << " to "
                  << target << std::endl;

        // Also copy bootstrap files and skills if they exist alongside the
        // config template
        fs::path template_base = template_path.parent_path().parent_path();
        fs::path workspace = fs::path(get_default_workspace());

        const std::vector<std::string> bootstrap_files = {
            "AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"};

        for (const auto &fname : bootstrap_files) {
          fs::path src = template_base / fname;
          fs::path dst = workspace / fname;
          if (fs::exists(src) && !fs::exists(dst)) {
            fs::copy_file(src, dst);
            std::cout << "Bootstrap file copied: " << fname << std::endl;
          }
        }

        // Copy skills directory
        fs::path src_skills = template_base / "skills";
        fs::path dst_skills = workspace / "skills";
        if (fs::exists(src_skills) && !fs::exists(dst_skills)) {
          fs::copy(src_skills, dst_skills,
                   fs::copy_options::recursive |
                       fs::copy_options::overwrite_existing);
          std::cout << "Skills directory copied from template." << std::endl;
        }

      } catch (const std::exception &e) {
        std::cerr << "Failed to copy initial workspace files: " << e.what()
                  << std::endl;
      }
    }
  }

  void bootstrap_workspace() { bootstrap_workspace(memory_workspace()); }

  static void bootstrap_workspace(const std::string &workspace) {
    fs::path ws = workspace;
    try {
      if (!fs::exists(ws))
        fs::create_directories(ws);
      if (!fs::exists(ws / "memory"))
        fs::create_directories(ws / "memory");
      if (!fs::exists(ws / "sessions"))
        fs::create_directories(ws / "sessions");
      if (!fs::exists(ws / "skills"))
        fs::create_directories(ws / "skills");
    } catch (const std::exception &e) {
      std::cerr << "Failed to bootstrap workspace: " << e.what() << std::endl;
    }
  }

private:
  Config() = default;
  YAML::Node config_;
  std::string actual_config_path_;

  template <typename T>
  T get(const std::string &section, const std::string &key,
        T default_val) const {
    if (config_[section] && config_[section][key]) {
      return config_[section][key].as<T>();
    }
    return default_val;
  }
};
