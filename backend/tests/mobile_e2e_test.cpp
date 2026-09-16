// End-to-end test of the mobile engine (C ABI) on desktop.
// Statically links the core sources with MC_MOBILE — the same code path the
// Android/iOS app runs, driven like the Rust host does.
//
// Usage: mc_e2e <workspace_dir> <message>
// Expects a mock OpenAI-compatible server (see MOBILE.md) configured in
// <workspace_dir>/config.yaml. Passes if the agent's exec tool call runs
// through the sandboxed mobile shell and its output reaches the driver.
#include "mobile/agent_api.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

static std::mutex g_mtx;
static std::condition_variable g_cv;
static std::atomic<int> g_done{0};
static bool g_got_tool_end = false;
static std::string g_tool_output;

static void on_event(void *user, const char *type, const char *content) {
  (void)user;
  std::lock_guard<std::mutex> lk(g_mtx);
  // Note: tool output may be multi-line; print it as its own block.
  if (std::string(type) == "tool_end") {
    std::printf("[event] tool_end:\n%s\n", content ? content : "");
    g_got_tool_end = true;
    g_tool_output = content ? content : "";
  } else {
    std::printf("[event] %s: %s\n", type, content ? content : "");
  }
  if (std::string(type) == "done" || std::string(type) == "error") {
    g_done = 1;
    g_cv.notify_all();
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::printf("usage: %s <workspace_dir> <message>\n", argv[0]);
    return 2;
  }
  const char *workspace = argv[1];
  const char *message = argv[2];

  mc_engine *eng = mc_engine_create(workspace, nullptr);
  if (!eng) {
    std::printf("FAIL: mc_engine_create: %s\n", mc_last_error());
    return 1;
  }
  std::printf("engine created (version %s, running=%d)\n", mc_version(),
              mc_is_running(eng));

  mc_send_message(eng, "e2e-test", message, on_event, nullptr);

  {
    std::unique_lock<std::mutex> lk(g_mtx);
    bool ok = g_cv.wait_for(lk, std::chrono::seconds(60),
                            [] { return g_done == 1; });
    if (!ok) {
      std::printf("FAIL: timed out waiting for done\n");
      mc_engine_destroy(eng);
      return 1;
    }
  }

  mc_engine_destroy(eng);

  // Verify the tool actually ran through the mobile shell in the sandbox.
  bool pass = g_got_tool_end && !g_tool_output.empty();
  std::printf(pass ? "E2E PASS: exec ran via mobile shell\n"
                   : "E2E FAIL: no tool output received\n");
  return pass ? 0 : 1;
}
