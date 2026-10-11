// Travel-tool trigger test: natural-language trip request (no explicit
// "use your tools" instruction), mirroring the reported user scenario:
//   "asking for travel to tibet for 7 days, but did not trigger the travel tool plugin"
// Usage: travel_trigger_test <workspace_dir>  (workspace must contain plugins/travel/)
#include "mobile/agent_api.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static std::atomic<bool> g_done{false};

static void on_event(void *user, const char *type, const char *content) {
  (void)user;
  std::string t = type ? type : "";
  if (t == "token")
    return;
  printf("[event] %s: %.300s\n", t.c_str(), content ? content : "");
  fflush(stdout);
  if (t == "done" || t == "error")
    g_done.store(true);
}

int main(int argc, char **argv) {
  const char *workspace =
      argc > 1 ? argv[1] : "C:/msys64/tmp/repro-heap-full";
  printf("travel_trigger_test: workspace=%s\n", workspace);
  fflush(stdout);

  mc_engine *eng = mc_engine_create(workspace, nullptr);
  if (!eng) {
    printf("FAIL: mc_engine_create: %s\n", mc_last_error());
    return 1;
  }

  if (mc_set_string(eng, "conversation", "provider", "openai") != 0 ||
      mc_set_string(eng, "conversation", "endpoint",
                    "http://localhost:8809/v1/chat/completions") != 0 ||
      mc_set_string(eng, "conversation", "model",
                    "unsloth/Qwen3.8-27B-GGUF:UD-Q5_K_XL") != 0 ||
      mc_set_string(eng, "conversation", "api_key", "local") != 0) {
    printf("FAIL: config: %s\n", mc_last_error());
    mc_engine_destroy(eng);
    return 1;
  }

  // Distillation via the same OpenAI-compatible endpoint (no MNN).
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

  printf("sending natural-language Tibet trip request...\n");
  fflush(stdout);
  mc_send_message(eng, "travel-trigger",
                  "I want to take a 7-day trip to Tibet starting around "
                  "November 1st. I'll fly from Shanghai. Make me a complete "
                  "trip plan: flights, day-by-day itinerary and a budget.",
                  on_event, nullptr);

  for (int i = 0; i < 420 && !g_done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (i % 30 == 0)
      printf("... waiting (%ds)\n", i), fflush(stdout);
  }

  printf("%s\n", g_done.load() ? "TURN COMPLETE" : "TIMED OUT");
  mc_engine_destroy(eng);
  printf("destroyed cleanly\n");
  return g_done.load() ? 0 : 2;
}
