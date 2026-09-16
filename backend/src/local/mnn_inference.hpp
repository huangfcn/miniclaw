#pragma once
// MnnInference — process-wide local inference runtime backed by Alibaba MNN.
//
// Two independent model slots, each with its own lazily-loaded model:
//   - "llm"       : chat / summarization (e.g. Qwen3-1.7B-MNN)
//   - "embedding" : sentence embeddings (e.g. BGE-M3 converted via llmexport.py)
//
// Model files live in the workspace (downloaded by the mobile app, see
// docs/LOCAL_MODELS.md):
//   <workspace>/models/<model-dir>/llm_config.json (+ llm.mnn, llm.mnn.weight,
//   tokenizer.txt)
//
// Threading contract: chat() / embed_text() / load_*() are BLOCKING and may
// run for seconds to minutes. Callers must invoke them from a worker
// std::thread and resume their fiber afterwards (Agent does this). A global
// mutex serializes all MNN work — the engine's turns are serialized anyway,
// but subagents + distillation could in principle overlap.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct AgentEvent;
struct LLMResponse;
struct Message;

class MnnInference {
 public:
  static MnnInference &instance();

  struct SlotStatus {
    bool loaded = false;
    std::string model_dir;  // resolved directory ("" if not configured)
    std::string message;    // last error / detail, empty when healthy
  };

  // Status for the C ABI (mc_local_status) and mobile UIs.
  SlotStatus llm_status() const;
  SlotStatus embedding_status() const;

  // One chat completion over the full message list. Blocking.
  // Streams {"token", chunk} events through on_event as they are generated.
  // Returns a response with .content set (no tool calls — local models do
  // not do function calling; the agent loop treats it as a final answer).
  LLMResponse chat(const std::vector<Message> &messages,
                   const std::function<void(const AgentEvent &)> &on_event,
                   std::string *error);

  // Embed one text. Blocking. Returns the raw (unnormalized) vector;
  // Agent::embed applies MRL truncation + L2 normalization afterwards.
  std::vector<float> embed_text(const std::string &text, std::string *error);

  // Eager preload (blocking). Used by mc_local_load so the first real call
  // is fast. Returns true on success.
  bool load_llm(std::string *error);
  bool load_embedding(std::string *error);

 private:
  MnnInference() = default;

  struct Slot {
    mutable std::mutex mutex;  // serializes inference for this slot
    void *handle = nullptr;  // Llm* / Embedding* (type lives in the .cpp)
    bool loaded = false;
    std::string dir;       // resolved model directory at load time
    std::string message;   // last error
  };

  Slot llm_;
  Slot emb_;

  // Global serialization: MNN runtimes are not safe to drive concurrently
  // from two threads (shared executor / Metal command queues).
  mutable std::mutex global_mutex_;

  void *ensure_llm(std::string *error);
  void *ensure_embedding(std::string *error);
  static std::string resolve_dir(const std::string &cfg_value);
};
