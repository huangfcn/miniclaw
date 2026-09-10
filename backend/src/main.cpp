// ─────────────────────────────────────────────────────────────────────────────
// miniclaw console server (desktop sidecar)
//
// Standalone console app that drives the engine through the *same* C ABI
// (mc_engine_*, see src/mobile/agent_api.h) that Android/iOS use in-process.
// It links libminiclaw_core — the shared library mobile embeds — and waits
// on a port with the classic HTTP/SSE endpoints:
//
//   GET  /api/health            → "OK"
//   POST /api/shutdown          → graceful shutdown
//   POST /api/chat              → SSE stream of {type, content} events
//   POST /v1/chat/completions   → OpenAI-compatible SSE stream
//
// The HTTP server itself lives inside libminiclaw_core (FiberPool); on
// desktop builds the FiberNodes bind the port from config (`server.port`,
// default 9000). This process is a thin host: create engine → wait → destroy.
//
// Usage:
//   miniclaw [workspace_dir]
//
// Environment:
//   WORKSPACE_DIR  workspace override (same as the old desktop binary)
//   MC_PORT        port override (takes precedence over config server.port)
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "agent/shutdown.hpp"
#include "mobile/agent_api.h"

namespace fs = std::filesystem;

#if defined(_WIN32)
#include <windows.h>
static void write_stderr(const char *msg) {
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg,
            static_cast<DWORD>(std::strlen(msg)), &written, nullptr);
}
#else
#include <unistd.h>
static void write_stderr(const char *msg) {
  const size_t len = std::strlen(msg);
  ssize_t off = 0;
  while (off < static_cast<ssize_t>(len)) {
    ssize_t n = write(STDERR_FILENO, msg + off, len - off);
    if (n <= 0) break;
    off += n;
  }
}
#endif

static std::atomic<long long> last_ctrl_c_timestamp{0};

// First Ctrl-C: graceful shutdown. Second within 1s: force exit.
static void on_ctrl_c() {
  auto now = std::chrono::steady_clock::now();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count();
  auto last = last_ctrl_c_timestamp.exchange(ms);

  if (ms - last <= 1000 && last != 0) {
    write_stderr("\nSecond Ctrl-C within 1s, force exiting...\n");
    _exit(1);
  }
  write_stderr("\nShutting down gracefully (Ctrl-C again within 1s to force "
               "exit)...\n");
  miniclaw_trigger_shutdown();
}

#if defined(_WIN32)
static BOOL WINAPI console_handler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
    on_ctrl_c();
    return TRUE;
  }
  return FALSE;
}
#else
#include <csignal>
static void signal_handler(int /*signum*/) { on_ctrl_c(); }
#endif

int main(int argc, char **argv) {
  // ── Workspace resolution (same precedence as the old desktop binary) ──
  std::string workspace;
  if (argc > 1 && argv[1][0]) {
    workspace = argv[1];
  } else if (const char *env_ws = std::getenv("WORKSPACE_DIR")) {
    workspace = env_ws;
  } else {
    // Same resolution order as the Tauri host (get_miniclaw_path in lib.rs):
    // USERPROFILE first on Windows, then HOME — so the sidecar always lands
    // in the directory Tauri just created.
#if defined(_WIN32)
    const char *home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
#else
    const char *home = std::getenv("HOME");
#endif
    workspace = std::string(home ? home : ".") + "/.miniclaw";
  }

  // Port: the core reads `server.port` from config; MC_PORT env var
  // overrides it (see Config::server_port()).

  fs::create_directories(workspace);

  // ── Create the engine via the shared C ABI (same call as mobile) ──────
  mc_engine *engine = mc_engine_create(workspace.c_str(), nullptr);
  if (!engine) {
    std::cerr << "miniclaw: engine init failed: " << mc_last_error()
              << std::endl;
    return 1;
  }

  // The engine already installed its spdlog logger; print a startup banner.
  printf("miniclaw console server v%s\n", mc_version());
  printf("  workspace: %s\n", fs::absolute(workspace).lexically_normal()
                                   .string()
                                   .c_str());
  printf("  config:    %s/config.yaml\n", workspace.c_str());
  printf("  http:      waiting on port (config server.port, default 9000; "
         "override with MC_PORT)\n");
  fflush(stdout);

  // ── Signal handling ────────────────────────────────────────────────────
#if defined(_WIN32)
  SetConsoleCtrlHandler(console_handler, TRUE);
#else
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
#endif

  // Block until shutdown is requested — either via Ctrl-C above or via the
  // POST /api/shutdown HTTP endpoint (which calls miniclaw_trigger_shutdown
  // inside the core).
  miniclaw_wait_for_shutdown();

  printf("\nminiclaw console server: shutting down...\n");
  fflush(stdout);
  mc_engine_destroy(engine);
  printf("miniclaw console server: exit.\n");
  return 0;
}
