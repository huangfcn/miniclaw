// Repro for the post-chat heap corruption (0xC0000374) seen in the server:
//   conversation = openai-compatible llama.cpp on :8809 (tool calling enabled)
//   memory/distillation = same OpenAI-compatible API (no local MNN inference)
//
// The scenario mirrors the crashing server run:
//   1. chat request that triggers travel + web_search + web_fetch tool calls
//      (exercises: SSE curl path, QuickJS plugin + js_http_fetch, web tools)
//   2. at end of turn, L1->L2 distillation runs via the API provider
//      (detached std::thread + fiber resume)
//
// Usage: repro_heap <workspace_dir>
// The workspace must contain plugins/travel/ (copied from ~/.miniclaw).

#include "mobile/agent_api.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static std::atomic<int> g_turns_done{0};
static std::atomic<int> g_target{0};

static void on_event(void *user, const char *type, const char *content) {
  (void)user;
  std::string t = type ? type : "";
  if (t == "token") {
    // Tokens are noisy; just count them.
  } else {
    printf("[event] %s: %.200s\n", t.c_str(), content ? content : "");
    fflush(stdout);
  }
  if (t == "done" || t == "error") g_turns_done.fetch_add(1);
}

int main(int argc, char **argv) {
  const char *workspace =
      argc > 1 ? argv[1] : "C:/msys64/tmp/repro-heap-ws";
  printf("repro_heap: workspace=%s\n", workspace);
  fflush(stdout);

  mc_engine *eng = mc_engine_create(workspace, nullptr);
  if (!eng) {
    printf("FAIL: mc_engine_create: %s\n", mc_last_error());
    return 1;
  }
  printf("engine created (running=%d)\n", mc_is_running(eng));

  // Conversation: llama.cpp OpenAI-compatible endpoint with tool calling.
  if (mc_set_string(eng, "conversation", "provider", "openai") != 0 ||
      mc_set_string(eng, "conversation", "endpoint",
                    "http://localhost:8809/v1/chat/completions") != 0 ||
      mc_set_string(eng, "conversation", "model",
                    "unsloth/Qwen3.8-27B-GGUF:UD-Q5_K_XL") != 0 ||
      mc_set_string(eng, "conversation", "api_key", "local") != 0) {
    printf("FAIL: conversation config: %s\n", mc_last_error());
    mc_engine_destroy(eng);
    return 1;
  }

  // Memory distillation: OpenAI-compatible API (llama.cpp on :8809), same as
  // conversation. No local MNN model inference per the current architecture.
  if (mc_set_string(eng, "memory", "provider", "openai") != 0 ||
      mc_set_string(eng, "memory", "endpoint",
                    "http://localhost:8809/v1/chat/completions") != 0 ||
      mc_set_string(eng, "memory", "model",
                    "unsloth/Qwen3.8-27B-GGUF:UD-Q5_K_XL") != 0 ||
      mc_set_string(eng, "memory", "api_key", "local") != 0) {
    printf("FAIL: memory config: %s\n", mc_last_error());
    mc_engine_destroy(eng);
    return 1;
  }

  // Force distillation to trigger on this short session (message_count mode
  // with a tiny threshold).
  mc_set_string(eng, "memory", "l1_distillation_trigger", "message_count");
  mc_set_string(eng, "memory", "l1_to_l2_threshold", "2");

  printf("config set; sending tool-heavy message\n");
  fflush(stdout);

  // Same shape as the crashing request: triggers travel + web_search +
  // web_fetch, then a final answer, then API distillation.
  mc_send_message(eng, "repro-heap",
                  "Use the travel tool first to search for flights from "
                  "Shanghai to Lhasa, then plan a 3-day trip. Use your tools.",
                  on_event, nullptr);

  auto wait_turns = [&](const char *label, int target, int timeout_s) {
    for (int i = 0; i < timeout_s && g_turns_done.load() < target; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (i % 30 == 0)
        printf("... waiting on %s (%ds, done=%d/%d)\n", label, i,
               g_turns_done.load(), target),
            fflush(stdout);
    }
    return g_turns_done.load() >= target;
  };

  // Turn 1: tool-heavy (travel + web_search + web_fetch + API distillation).
  bool ok1 = wait_turns("turn 1", 1, 600);
  printf("turn 1 %s; sending turn 2\n", ok1 ? "finished" : "TIMED OUT");
  fflush(stdout);

  // Turn 2: exercises repeat distillation on the same process
  // (the crashing server had handled several turns before).
  mc_send_message(eng, "repro-heap",
                  "Summarize the trip plan in two sentences.", on_event,
                  nullptr);
  bool ok2 = wait_turns("turn 2", 2, 600);

  printf((ok1 && ok2) ? "REPRO-HEAP SURVIVED (no crash)\n"
                      : "REPRO-HEAP INCOMPLETE (ok1=%d ok2=%d)\n",
         (int)ok1, (int)ok2);

  // Give any late corruption a moment to surface, then exercise the heap.
  std::this_thread::sleep_for(std::chrono::seconds(5));
  mc_engine_destroy(eng);
  printf("engine destroyed cleanly\n");
  return (ok1 && ok2) ? 0 : 2;
}
