// ─────────────────────────────────────────────────────────────────────────────
// miniclaw mobile C ABI — implementation
//
// Embeds the C++ Agent core for in-process use on Android/iOS (Tauri mobile,
// or any other host). See agent_api.h for the contract.
// ─────────────────────────────────────────────────────────────────────────────

#include "agent_api.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <cstdlib>

#include <curl/curl.h>
#include <fiber.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "agent.hpp"
#include "agent/cron_service.hpp"
#include "agent/fiber_pool.hpp"
#include "config.hpp"

namespace {

std::string& last_error_storage() {
  static std::string s;
  return s;
}

void set_last_error(const std::string &msg) { last_error_storage() = msg; }

// Default config written on first launch if the app has no config.yaml yet.
// The UI (Tauri settings screen) can overwrite any of these via mc_set_config.
const char *kDefaultConfigYaml = R"(# miniclaw configuration (mobile)
server:
  port: 9000
  threads: 2

conversation:
  provider: openai
  model: gpt-4o-mini
  endpoint: https://api.openai.com/v1/chat/completions
  api_key: ""

memory:
  workspace: .
  l1_to_l2_threshold: 30
  context_window: 128000
  compaction_threshold: 0.8
  time: "13:00"
  provider: openai
  model: gpt-4o-mini
  endpoint: ""

embedding:
  provider: openai
  model: text-embedding-3-small
  endpoint: https://api.openai.com/v1/embeddings
  dimension: 1536

logging:
  level: info
  file: backend.log
  enabled: true
)";

// ── Bootstrap files seeded on first launch (mobile-tuned) ────────────────
// Android assets are read-only and iOS has no equivalent, so instead of
// copying bundled templates we generate these directly in the app sandbox.
// Each file is only written if missing — the agent/user can edit them later.

const char *kAgentsMd = R"(# Agent Instructions

You are a helpful AI assistant running on the user's phone. Be concise, accurate, and friendly.

## Guidelines

- Always explain what you're doing before taking actions
- Ask for clarification when the request is ambiguous
- Use tools to help accomplish tasks
- Remember important information in your memory files
- Keep answers short enough to read comfortably on a phone screen

## Environment

- You run inside the app sandbox; all tools only work within the workspace.
- The `exec` tool runs a small built-in POSIX-style shell (ls, cat, grep, find,
  wc, head, tail, sort, uniq, cut, mkdir, rm, cp, mv, touch, echo, ... with
  `|` pipelines). It has no external binaries, redirection, `&&`, or variables.

## Tools Available

You have access to:
- File operations (read_file, write_file, edit_file, list_dir)
- Web access (web_search, web_fetch)
- Memory recall (memory_search)
- Background tasks (spawn, cron)

## Memory

- `memory/MEMORY.md` — long-term facts (preferences, context, relationships)
- `memory/HISTORY.md` — append-only event log; use memory_search to recall past events

## Scheduled Reminders

When the user asks for a reminder or recurring task, use the `cron` tool:
```
cron(action: "add", schedule: "every 10s" | cron expression, task: "description")
cron(action: "list")
cron(action: "remove", job_id: "...")
```

**Do NOT just write reminders to MEMORY.md** — that won't trigger actual notifications.

## Heartbeat Tasks

`HEARTBEAT.md` is checked periodically. You can manage periodic tasks by editing this file:

- **Add a task**: Use `edit_file` to append new tasks to `HEARTBEAT.md`
- **Remove a task**: Use `edit_file` to remove completed or obsolete tasks

Task format examples:
```
- [ ] Check calendar and remind of upcoming events
- [ ] Scan inbox for urgent emails
```

When the user asks you to add a recurring/periodic task, update `HEARTBEAT.md` instead of creating a one-time reminder. Keep the file small to minimize token usage.
)";

const char *kSoulMd = R"(You prefer concise answers suited to a phone screen. Always prefer using tools to answer questions. Do not ask for confirmation before using tools for research or info-gathering tasks.)";

const char *kIdentityMd =
    "You are an advanced AI agent named miniclaw, running privately on the user's phone.";

const char *kUserMd = R"(# User Profile

Information about the user to help personalize interactions.

## Basic Information

- **Name**: (your name)
- **Timezone**: (your timezone, e.g., UTC+8)
- **Language**: (preferred language)

## Preferences

### Communication Style

- [ ] Casual
- [ ] Professional
- [ ] Technical

### Response Length

- [ ] Brief and concise
- [ ] Detailed explanations
- [ ] Adaptive based on question

### Technical Level

- [ ] Beginner
- [ ] Intermediate
- [ ] Expert

## Work Context

- **Primary Role**: (your role, e.g., developer, researcher)
- **Main Projects**: (what you're working on)
- **Tools You Use**: (IDEs, languages, frameworks)

## Topics of Interest

- 
- 
- 

## Special Instructions

(Any specific instructions for how the assistant should behave)

---

*Edit this file to customize miniclaw's behavior for your needs.*
)";

const char *kToolsMd = R"(# Available Tools

This document describes the tools available to miniclaw on this device.

## File Operations (sandboxed to the workspace)

### read_file
Read the contents of a file.
```
read_file(path: str) -> str
```

### write_file
Write content to a file (creates parent directories if needed).
```
write_file(path: str, content: str) -> str
```

### edit_file
Edit a file by replacing specific text.
```
edit_file(path: str, old_text: str, new_text: str) -> str
```

### list_dir
List contents of a directory.
```
list_dir(path: str) -> str
```

**Note:** All file paths are restricted to the app workspace. Shell commands
via `exec` are sandboxed to the same workspace — paths outside it are rejected.

## Web Access

### web_search
Search the web using Brave Search API.
```
web_search(query: str, count: int = 5) -> str
```

Returns search results with titles, URLs, and snippets. Requires `tools.web.search.apiKey` in config.

### web_fetch
Fetch and extract main content from a URL.
```
web_fetch(url: str, extractMode: str = "markdown", maxChars: int = 50000) -> str
```

## Memory

### memory_search
Search long-term memory (vector + keyword) for past context.
```
memory_search(query: str, limit: int = 5) -> str
```

### index_document
Index a document into the memory store.
```
index_document(path: str) -> str
```

## Background Tasks

### cron
Manage background tasks (add, remove, list).
```
cron(action: "add"|"remove"|"list", schedule: str, task: str, job_id: str) -> str
```

`schedule` is a cron expression or a relative interval like `every 10s`.

### spawn
Run a subagent for a background task.
)";

// Write each bootstrap file into the workspace if it does not exist yet.
static void seed_bootstrap_files(const std::string &workspace) {
  const std::pair<const char *, const char *> files[] = {
      {"AGENTS.md", kAgentsMd},
      {"SOUL.md", kSoulMd},
      {"IDENTITY.md", kIdentityMd},
      {"USER.md", kUserMd},
      {"TOOLS.md", kToolsMd},
  };
  for (const auto &[fname, content] : files) {
    fs::path p = fs::path(workspace) / fname;
    if (fs::exists(p))
      continue;
    try {
      std::ofstream f(p);
      f << content;
      spdlog::info("[mobile] seeded bootstrap file: {}", fname);
    } catch (const std::exception &e) {
      spdlog::warn("[mobile] could not seed {}: {}", fname, e.what());
    }
  }
}

class McEngine {
public:
  std::string workspace;
  std::string config_path;
  std::unique_ptr<Agent> agent;

  // Serializes agent turns: one turn at a time across all sessions.
  // A phone assistant rarely needs parallel turns, and this keeps session
  // files and the memory index simple to reason about.
  std::mutex turn_mutex;
  std::atomic<bool> running{false};

  void set_error(const std::string &msg) {
    spdlog::error("[mobile] {}", msg);
    set_last_error(msg);
  }
};

// Run one agent turn on a FiberPool fiber, serializing via the engine's
// turn mutex. The completion condition variable is signaled when the
// "done"/"error" event fires or the turn throws.
void run_turn(McEngine *engine, const std::string &session_id,
              const std::string &message, mc_event_cb cb, void *user_data) {
  std::mutex mtx;
  std::condition_variable cv;
  bool finished = false;

  AgentEventCallback on_event = [&mtx, &cv, &finished, cb, user_data](
                                     const AgentEvent &e) {
    if (cb) cb(user_data, e.type.c_str(), e.content.c_str());
    if (e.type == "done" || e.type == "error") {
      std::lock_guard<std::mutex> lock(mtx);
      finished = true;
      cv.notify_all();
    }
  };

  // Block until we own the turn slot, then dispatch to a fiber.
  engine->turn_mutex.lock();
  spawn_in_fiber([engine, session_id, message, on_event, cb, user_data, &mtx,
                  &cv, &finished]() {
    try {
      engine->agent->run(message, session_id, on_event);
    } catch (const std::exception &e) {
      if (cb) cb(user_data, "error", e.what());
      std::lock_guard<std::mutex> lock(mtx);
      finished = true;
      cv.notify_all();
    } catch (...) {
      if (cb) cb(user_data, "error", "Unknown error in agent turn");
      std::lock_guard<std::mutex> lock(mtx);
      finished = true;
      cv.notify_all();
    }
  });

  // Wait for the turn to finish before releasing the slot.
  {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&finished] { return finished; });
  }
  engine->turn_mutex.unlock();
}

} // namespace

// The C ABI exposes `struct mc_engine` (opaque); internally it is McEngine.
static inline McEngine *eng(mc_engine *e) { return reinterpret_cast<McEngine *>(e);
}
static inline mc_engine *uneng(McEngine *e) {
  return reinterpret_cast<mc_engine *>(e);
}

extern "C" {

mc_engine *mc_engine_create(const char *workspace_dir, const char *config_path) {
  if (!workspace_dir || !*workspace_dir) {
    set_last_error("workspace_dir must not be empty");
    return nullptr;
  }

  try {
    auto *engine = new McEngine();
    engine->workspace = workspace_dir;
    engine->config_path =
        (config_path && *config_path) ? config_path : (std::string(workspace_dir) + "/config.yaml");

    // Point the C++ core at this workspace. Config::memory_workspace()
    // honors WORKSPACE_DIR first; setting it before load() is safe here
    // because no other threads exist yet on a fresh process, and on mobile
    // the engine is created exactly once per app process.
#if defined(_WIN32)
    _putenv_s("WORKSPACE_DIR", engine->workspace.c_str());
#else
    setenv("WORKSPACE_DIR", engine->workspace.c_str(), 1);
#endif

    Config &cfg = Config::instance();
    cfg.load(engine->config_path);

    // First launch: write a default config so the UI has something to edit.
    {
      fs::path p = engine->config_path;
      if (!fs::exists(p)) {
        try {
          fs::create_directories(fs::absolute(p).parent_path());
          std::ofstream f(p);
          f << kDefaultConfigYaml;
          spdlog::info("[mobile] wrote default config to {}", p.string());
        } catch (const std::exception &e) {
          spdlog::warn("[mobile] could not write default config: {}", e.what());
        }
      }
    }

    // Logging: stdout goes to logcat on Android; also keep a file in the
    // app sandbox for debugging.
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    std::string level_str = cfg.logging_level();
    spdlog::level::level_enum log_level = spdlog::level::from_str(level_str);
    console_sink->set_level(log_level);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(console_sink);
    if (cfg.logging_enabled()) {
      std::string log_path =
          (fs::path(engine->workspace) / cfg.logging_file()).string();
      try {
        auto file_sink =
            std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path, false);
        file_sink->set_level(log_level);
        sinks.push_back(file_sink);
      } catch (...) {
        // Non-fatal: console-only logging.
      }
    }
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("mobile", sinks.begin(), sinks.end()));
    spdlog::set_level(log_level);

    cfg.bootstrap_workspace(engine->workspace);
    seed_bootstrap_files(engine->workspace);

    FiberGlobalStartup();

    engine->agent = std::make_unique<Agent>();

    int threads = cfg.server_threads();
    if (threads < 1) threads = 2;
    FiberPool::instance().init(threads, engine->agent.get());

    init_spawn_system();

    CronService::instance().init(engine->workspace);
    CronService::instance().start();

    engine->running = true;
    spdlog::info("[mobile] engine ready (workspace={})", engine->workspace);
    return uneng(engine);
  } catch (const std::exception &e) {
    set_last_error(std::string("mc_engine_create failed: ") + e.what());
    spdlog::error("[mobile] {}", last_error_storage());
    return nullptr;
  } catch (...) {
    set_last_error("mc_engine_create failed: unknown error");
    return nullptr;
  }
}

void mc_engine_destroy(mc_engine *engine) {
  auto *e = eng(engine);
  if (!e) return;
  try {
    e->running = false;
    CronService::instance().stop();
    FiberPool::instance().stop();
    curl_global_cleanup();
  } catch (const std::exception &ex) {
    spdlog::error("[mobile] error during shutdown: {}", ex.what());
  }
  delete e; // Agent, pools etc. torn down here
}

void mc_send_message(mc_engine *engine, const char *session_id,
                     const char *message, mc_event_cb cb, void *user_data) {
  auto *e = eng(engine);
  if (!e || !e->running || !message) return;

  std::string sid = session_id ? session_id : "default";
  std::string msg = message;

  // Detached worker: waits for the turn slot, then runs the turn on a fiber.
  std::thread([e, sid, msg = std::move(msg), cb, user_data]() {
    run_turn(e, sid, msg, cb, user_data);
  }).detach();
}

char *mc_get_config(mc_engine *engine) {
  auto *e = eng(engine);
  if (!e) return nullptr;
  try {
    fs::path p = e->config_path;
    if (!fs::exists(p)) return nullptr;
    std::ifstream f(p);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    char *out = static_cast<char *>(malloc(content.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, content.c_str(), content.size() + 1);
    return out;
  } catch (...) {
    set_last_error("mc_get_config failed");
    return nullptr;
  }
}

int mc_set_config(mc_engine *engine, const char *yaml_text) {
  auto *e = eng(engine);
  if (!e || !yaml_text) return -1;
  try {
    // Validate by parsing before touching disk.
    YAML::Load(yaml_text);

    fs::path p = e->config_path;
    fs::create_directories(fs::absolute(p).parent_path());
    std::ofstream f(p, std::ios::trunc);
    f << yaml_text;
    f.flush();
    if (f.fail()) {
      set_last_error("failed to write config file");
      return -1;
    }

    // Reload into the singleton. Note: Agent captured endpoint/model strings
    // at construction; conversation settings take effect on next app launch.
    // That is acceptable for a v1 mobile build — the UI documents this.
    Config::instance().load(e->config_path);
    return 0;
  } catch (const std::exception &e) {
    set_last_error(std::string("mc_set_config failed: ") + e.what());
    return -1;
  }
}

int mc_set_string(mc_engine *engine, const char *section, const char *key,
                  const char *value) {
  auto *e = eng(engine);
  if (!e || !section || !key) return -1;
  try {
    YAML::Node node = YAML::Load(kDefaultConfigYaml);
    // Start from the on-disk config if present so we don't clobber it.
    fs::path p = e->config_path;
    if (fs::exists(p)) {
      try {
        node = YAML::LoadFile(p.string());
      } catch (...) {
      }
    }
    node[section][key] = value ? value : "";

    std::ostringstream ss;
    ss << node;
    return mc_set_config(engine, ss.str().c_str());
  } catch (const std::exception &e) {
    set_last_error(std::string("mc_set_string failed: ") + e.what());
    return -1;
  }
}

int mc_is_running(mc_engine *engine) {
  auto *e = eng(engine);
  return (e && e->running) ? 1 : 0;
}

const char *mc_last_error(void) { return last_error_storage().c_str(); }

void mc_free_string(char *s) { free(s); }

const char *mc_version(void) { return "0.1.0"; }

} // extern "C"
