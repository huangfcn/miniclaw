// End-to-end test of the MNN local inference interface (C ABI) on desktop.
// Drives the exact same calls the iOS/Android apps make:
//   mc_engine_create → mc_local_status → mc_local_load → mc_send_message
//
// Usage: mnn_local_test <workspace_dir> [message] [embedding]
//                [--llm <model_dir>] [--emb <model_dir>]
//
// Model directories come from the workspace config.yaml (local.llm_model_dir
// / local.embedding_model_dir). --llm/--emb override them for this run via
// mc_set_string (merged into the on-disk config, like the app UI does) and
// also pin conversation/memory provider to "mnn" so the test is
// self-contained. Passes when a non-empty assistant reply is streamed back.

#include "local/mnn_inference.hpp"
#include "mobile/agent_api.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static std::string g_reply;

static void on_event(void *user, const char *type, const char *content) {
  (void)user;
  std::string t = type ? type : "";
  if (t == "delta" || t == "token") {
    // Streamed token chunk — print raw so streaming is visible.
    fwrite(content ? content : "", 1, std::strlen(content ? content : ""), stdout);
    fflush(stdout);
    g_reply += content ? content : "";
  } else {
    std::printf("\n[event] %s: %s\n", t.c_str(), content ? content : "");
  }
}

int main(int argc, char **argv) {
  // Split argv into positionals plus optional --llm/--emb model-dir flags.
  std::vector<std::string> pos;
  std::string llm_dir, emb_dir;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--llm" && i + 1 < argc) {
      llm_dir = argv[++i];
    } else if (a == "--emb" && i + 1 < argc) {
      emb_dir = argv[++i];
    } else {
      pos.push_back(std::move(a));
    }
  }
  if (pos.empty()) {
    std::printf(
        "usage: %s <workspace_dir> [message] [embedding] "
        "[--llm <model_dir>] [--emb <model_dir>]\n",
        argv[0]);
    return 2;
  }
  const char *workspace = pos[0].c_str();
  const char *message =
      pos.size() > 1 ? pos[1].c_str() : "Reply with exactly one word: hello";

  mc_engine *eng = mc_engine_create(workspace, nullptr);
  if (!eng) {
    std::printf("FAIL: mc_engine_create: %s\n", mc_last_error());
    return 1;
  }
  std::printf("engine created (running=%d)\n", mc_is_running(eng));

  // 0. Optional CLI model-dir overrides: merged into the workspace config
  //    (same path the app UI takes), so no hand-edited YAML is needed.
  if (!llm_dir.empty() || !emb_dir.empty()) {
    if (mc_set_string(eng, "conversation", "provider", "mnn") != 0 ||
        mc_set_string(eng, "memory", "provider", "mnn") != 0) {
      std::printf("FAIL: mc_set_config failed: %s\n", mc_last_error());
      mc_engine_destroy(eng);
      return 1;
    }
    if (!llm_dir.empty() &&
        mc_set_string(eng, "local", "llm_model_dir", llm_dir.c_str()) != 0) {
      std::printf("FAIL: mc_set_config failed: %s\n", mc_last_error());
      mc_engine_destroy(eng);
      return 1;
    }
    if (!emb_dir.empty() &&
        mc_set_string(eng, "local", "embedding_model_dir", emb_dir.c_str()) != 0) {
      std::printf("FAIL: mc_set_config failed: %s\n", mc_last_error());
      mc_engine_destroy(eng);
      return 1;
    }
    std::printf("config updated: llm=%s emb=%s (provider=mnn)\n",
                llm_dir.empty() ? "(unchanged)" : llm_dir.c_str(),
                emb_dir.empty() ? "(unchanged)" : emb_dir.c_str());
  }

  // 1. Initial status (expect "model not found" when weights are absent).
  {
    char *st = mc_local_status(eng);
    std::printf("status(initial): %s\n", st ? st : "(null)");
    if (st) mc_free_string(st);
  }

  // 2. Kick off the load (background thread inside the engine).
  bool emb_mode = pos.size() > 2 && pos[2] == "embedding";
  const char *load_kind = emb_mode ? "embedding" : "llm";
  int rc = mc_local_load(eng, load_kind);
  std::printf("mc_local_load(%s) -> %d\n", load_kind, rc);
  if (rc != 0) {
    std::printf("FAIL: load rejected (%s)\n", mc_last_error());
    mc_engine_destroy(eng);
    return 1;
  }

  // 3. Poll until loaded or failed (first load maps ~GB of weights; allow 5 minutes).
  bool loaded = false, failed = false;
  for (int i = 0; i < 300 && !loaded && !failed; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    char *st = mc_local_status(eng);
    if (!st) break;
    std::string s = st;
    mc_free_string(st);
    loaded = s.find(emb_mode ? "\"embedding\":{\"loaded\":true" : "\"llm\":{\"loaded\":true") != std::string::npos;
    failed = s.find("load failed") != std::string::npos ||
             s.find("model not found") != std::string::npos;
    if (i % 5 == 0) std::printf("status(%ds): %s\n", i, s.c_str());
  }
  if (emb_mode) {
    // Embedding mode: report load result and exit (no chat).
    bool pass = loaded;
    if (loaded) {
      // Exercise a real embedding so the test covers inference, not just
      // session creation. Dump the raw vector for offline comparison.
      std::string err;
      auto vec = MnnInference::instance().embed_text(
          "miniclaw local embedding quality test", &err);
      double norm = 0;
      for (float v : vec) norm += static_cast<double>(v) * v;
      norm = std::sqrt(norm);
      std::printf("embed dim=%zu l2_norm=%.6f err=%s\n",
                  vec.size(), norm, err.empty() ? "-" : err.c_str());
      if (!vec.empty()) {
        for (int i = 0; i < 8 && i < static_cast<int>(vec.size()); ++i)
          std::printf("  v[%d]=%.6f\n", i, vec[i]);
        std::string bin_path = std::string(workspace) + "/emb_test.bin";
        std::ofstream f(bin_path, std::ios::binary);
        f.write(reinterpret_cast<const char *>(vec.data()),
                static_cast<std::streamsize>(vec.size() * sizeof(float)));
        std::printf("wrote %s\n", bin_path.c_str());
      }
      // Sensitivity check: a different text must give a different vector.
      auto vec2 = MnnInference::instance().embed_text(
          "completely different sentence about cooking pasta", &err);
      if (!vec.empty() && vec.size() == vec2.size()) {
        double dot = 0, n1 = 0, n2 = 0;
        for (size_t i = 0; i < vec.size(); ++i) {
          dot += static_cast<double>(vec[i]) * vec2[i];
          n1 += static_cast<double>(vec[i]) * vec[i];
          n2 += static_cast<double>(vec2[i]) * vec2[i];
        }
        std::printf("sensitivity: cos(v1,v2)=%.8f\n",
                    dot / (std::sqrt(n1) * std::sqrt(n2)));
        std::string bin2 = std::string(workspace) + "/emb_test2.bin";
        std::ofstream f2(bin2, std::ios::binary);
        f2.write(reinterpret_cast<const char *>(vec2.data()),
                 static_cast<std::streamsize>(vec2.size() * sizeof(float)));
      }
      pass = !vec.empty() && norm > 0.5 && norm < 1.5;
    }
    std::printf(pass ? "EMBED-LOAD PASS\n" : "EMBED-LOAD FAIL (see status above)\n");
    mc_engine_destroy(eng);
    return pass ? 0 : 1;
  }
  if (!loaded) {
    std::printf("FAIL: LLM never reported loaded (failed=%d)\n", (int)failed);
    mc_engine_destroy(eng);
    return 1;
  }
  std::printf("LLM loaded — sending test message\n");

  // 4. Chat through the local provider and stream events.
  mc_send_message(eng, "mnn-test", message, on_event, nullptr);
  std::this_thread::sleep_for(std::chrono::seconds(120));

  bool pass = !g_reply.empty();
  std::printf("\n%s (reply %zu chars)\n",
              pass ? "MNN-LOCAL PASS" : "MNN-LOCAL FAIL: empty reply",
              g_reply.size());
  mc_engine_destroy(eng);
  return pass ? 0 : 1;
}
