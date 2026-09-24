// MnnInference — MNN-backed local inference (see mnn_inference.hpp).
//
// Always compiled (agent.cpp references the class unconditionally); with
// MC_HAVE_MNN undefined every method reports "MNN support not compiled".

#include "local/mnn_inference.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

#include "../agent/agent_types.hpp"
#include "../config.hpp"

#ifdef MC_HAVE_MNN
#include <llm/llm.hpp>  // MNN::Transformer::{Llm, Embedding, ChatMessages}

namespace {

// streambuf that turns MNN's per-token `*os << piece << flush` pattern into
// discrete chunk callbacks: characters accumulate until the next flush, at
// which point the pending text is emitted as one chunk (see
// speculative_decoding/generate.cpp — one write + one flush per token).
class TokenStreamBuf : public std::streambuf {
 public:
  explicit TokenStreamBuf(std::function<void(const std::string&)> on_chunk)
      : on_chunk_(std::move(on_chunk)) {}

 protected:
  // No internal buffer region: every character written to this streambuf
  // lands in overflow() (per-char calls are negligible next to decode time).
  int overflow(int_type c) override {
    if (c != traits_type::eof()) pending_ += traits_type::to_char_type(c);
    return traits_type::not_eof(c);
  }

  int sync() override {
    if (!pending_.empty()) {
      if (on_chunk_) on_chunk_(pending_);
      pending_.clear();
    }
    return 0;
  }

 private:
  std::function<void(const std::string&)> on_chunk_;
  std::string pending_;
};

}  // namespace
#endif  // MC_HAVE_MNN

MnnInference &MnnInference::instance() {
  static MnnInference inst;
  return inst;
}

std::string MnnInference::resolve_dir(const std::string &cfg_value) {
  namespace fs = std::filesystem;
  if (cfg_value.empty()) return "";
  fs::path p(cfg_value);
  if (p.is_absolute()) return fs::absolute(p).lexically_normal().string();
  return (fs::path(Config::instance().memory_workspace()) / p)
      .lexically_normal()
      .string();
}

// When a section's provider is "mnn", its `endpoint:` field doubles as the
// local model folder (e.g. memory.endpoint: ~/.miniclaw/models/qwen3.5-4b).
// Remote endpoints (http://, https://) are never treated as directories.
static bool endpoint_is_local_dir(const std::string &endpoint) {
  if (endpoint.empty()) return false;
  return endpoint.rfind("http://", 0) != 0 &&
         endpoint.rfind("https://", 0) != 0;
}

MnnInference::SlotStatus MnnInference::llm_status() const {
  std::lock_guard<std::mutex> lock(llm_.mutex);
  SlotStatus s;
  s.loaded = llm_.loaded;
  s.model_dir = llm_.dir;
  s.message = llm_.message;
  return s;
}

MnnInference::SlotStatus MnnInference::embedding_status() const {
  std::lock_guard<std::mutex> lock(emb_.mutex);
  SlotStatus s;
  s.loaded = emb_.loaded;
  s.model_dir = emb_.dir;
  s.message = emb_.message;
  return s;
}

#ifdef MC_HAVE_MNN

// MNN's LLM engine defaults to backend_type "cpu" (llmconfig.hpp). Apple
// builds compile MNN with MNN_METAL=ON, so the default (local.llm_backend:
// auto) prefers the Metal GPU backend there; "cpu"/"metal" force a choice.
// A failed Metal load always retries on CPU, so a model that loads is one
// that loads. Works for both Llm and Embedding (Embedding : public Llm
// shares initRuntime()).
static bool load_prefer_metal(MNN::Transformer::Llm *model) {
  std::string want = Config::instance().local_llm_backend();
  for (auto &c : want) c = (char)std::tolower((unsigned char)c);
#if defined(__APPLE__)
  bool try_metal_first = want == "metal" || want == "auto";
#else
  bool try_metal_first = want == "metal"; // Metal is not built in elsewhere
#endif
  if (try_metal_first) {
    model->set_config("{\"backend_type\": \"metal\"}");
    if (model->load()) return true;
    std::string metal_log = model->getLog();
    model->set_config("{\"backend_type\": \"cpu\"}");
    if (model->load()) {
      spdlog::warn("MNN: Metal backend load failed ({}); falling back to CPU",
                   metal_log.empty() ? std::string("no log") : metal_log);
      return true;
    }
    return false;
  }
  return model->load();
}

void *MnnInference::ensure_llm(std::string *error) {
  std::lock_guard<std::mutex> lock(llm_.mutex);
  if (llm_.loaded) return llm_.handle;

  // Directory resolution order (first hit wins):
  //   1. conversation.endpoint, when conversation.provider == mnn
  //   2. memory.endpoint,      when memory.provider == mnn
  //   3. local.llm_model_dir   (legacy / fallback key)
  const auto &cfg = Config::instance();
  std::string dir_cfg;
  if (cfg.conversation_provider() == "mnn" &&
      endpoint_is_local_dir(cfg.conversation_endpoint())) {
    dir_cfg = cfg.conversation_endpoint();
  } else if (cfg.memory_distillation_provider() == "mnn" &&
             endpoint_is_local_dir(cfg.memory_distillation_endpoint())) {
    dir_cfg = cfg.memory_distillation_endpoint();
  } else {
    dir_cfg = cfg.local_llm_model_dir();
  }
  // Both sections may point at different folders, but the LLM slot is a
  // single loaded model — surface the conflict instead of failing silently.
  if (cfg.conversation_provider() == "mnn" &&
      cfg.memory_distillation_provider() == "mnn") {
    std::string conv_dir = endpoint_is_local_dir(cfg.conversation_endpoint())
                               ? cfg.conversation_endpoint()
                               : "";
    std::string mem_dir = endpoint_is_local_dir(cfg.memory_distillation_endpoint())
                              ? cfg.memory_distillation_endpoint()
                              : "";
    if (!conv_dir.empty() && !mem_dir.empty() && conv_dir != mem_dir) {
      spdlog::warn(
          "MNN LLM: conversation.endpoint ({}) and memory.endpoint ({}) differ; "
          "one shared model slot will use the first", conv_dir, mem_dir);
    }
  }
  llm_.dir = resolve_dir(dir_cfg);
  std::string config_path = llm_.dir + "/llm_config.json";
  if (!std::filesystem::exists(config_path)) {
    llm_.message = "model not found: " + config_path;
    spdlog::error("MNN LLM: {}", llm_.message);
    if (error) *error = llm_.message;
    return nullptr;
  }

  spdlog::info("MNN LLM: loading {} ...", config_path);
  auto *llm = MNN::Transformer::Llm::createLLM(config_path);
  bool ok = false;
  try {
    ok = load_prefer_metal(llm);
  } catch (const std::exception &e) {
    llm_.message = std::string("load failed: ") + e.what();
  }
  if (!ok && llm_.message.empty()) {
    llm_.message = "load failed: " + llm->getLog();
  }
  if (!ok) {
    spdlog::error("MNN LLM: {}", llm_.message);
    MNN::Transformer::Llm::destroy(llm);
    if (error) *error = llm_.message;
    return nullptr;
  }

  llm_.handle = llm;
  llm_.loaded = true;
  llm_.message.clear();
  spdlog::info("MNN LLM: loaded from {}", llm_.dir);
  return llm_.handle;
}

void *MnnInference::ensure_embedding(std::string *error) {
  std::lock_guard<std::mutex> lock(emb_.mutex);
  if (emb_.loaded) return emb_.handle;

  // Directory resolution order (first hit wins):
  //   1. embedding.endpoint, when embedding.provider == mnn
  //   2. local.embedding_model_dir (legacy / fallback key)
  const auto &cfg = Config::instance();
  std::string dir_cfg = cfg.embedding_provider() == "mnn" &&
                              endpoint_is_local_dir(cfg.embedding_endpoint())
                          ? cfg.embedding_endpoint()
                          : cfg.local_embedding_model_dir();
  emb_.dir = resolve_dir(dir_cfg);
  std::string config_path = emb_.dir + "/llm_config.json";
  if (!std::filesystem::exists(config_path)) {
    emb_.message = "model not found: " + config_path;
    spdlog::error("MNN Embedding: {}", emb_.message);
    if (error) *error = emb_.message;
    return nullptr;
  }

  spdlog::info("MNN Embedding: loading {} ...", config_path);
  // load=false: we call load() ourselves to check its result.
  auto *emb = MNN::Transformer::Embedding::createEmbedding(config_path, false);
  bool ok = false;
  try {
    ok = load_prefer_metal(emb);
  } catch (const std::exception &e) {
    emb_.message = std::string("load failed: ") + e.what();
  }
  if (!ok && emb_.message.empty()) {
    emb_.message = "load failed: " + emb->getLog();
  }
  if (!ok) {
    spdlog::error("MNN Embedding: {}", emb_.message);
    MNN::Transformer::Llm::destroy(emb);
    if (error) *error = emb_.message;
    return nullptr;
  }

  emb_.handle = emb;
  emb_.loaded = true;
  emb_.message.clear();
  spdlog::info("MNN Embedding: loaded from {} (dim={})", emb_.dir, emb->dim());
  return emb_.handle;
}

LLMResponse MnnInference::chat(
    const std::vector<Message> &messages,
    const std::function<void(const AgentEvent&)> &on_event,
    std::string *error) {
  LLMResponse resp;
  std::lock_guard<std::mutex> global(global_mutex_);

  auto *llm = static_cast<MNN::Transformer::Llm *>(ensure_llm(error));
  if (!llm) return resp;
  std::lock_guard<std::mutex> slot(llm_.mutex);

  // Map miniclaw messages -> MNN chat template roles. Local models do not
  // do function calling: assistant tool_calls are dropped, tool results are
  // folded into user messages as labeled text.
  MNN::Transformer::ChatMessages prompts;
  for (const auto &m : messages) {
    if (m.role == "tool") {
      std::string label = m.name.empty() ? m.tool_call_id : m.name;
      prompts.emplace_back(
          "user", "[tool result: " + label + "]\n" + m.content);
    } else if (m.role == "assistant") {
      if (!m.content.empty()) prompts.emplace_back("assistant", m.content);
      // tool_calls_json is intentionally dropped
    } else {
      prompts.emplace_back(m.role, m.content);  // system / user
    }
  }

  // Fresh context per call: the agent always passes the full history and
  // MNN keeps its own KV history across response() calls — without a reset
  // the prompt would be duplicated. (Prompt caching can replace this later.)
  llm->reset();

  std::string content;
  TokenStreamBuf buf([&](const std::string &chunk) {
    content += chunk;
    if (on_event) on_event({"token", chunk});
  });
  std::ostream stream(&buf);
  try {
    llm->response(prompts, &stream, /*end_with=*/"", /*max_new_tokens=*/-1);
    // Emit anything left without a trailing flush.
    stream.flush();
  } catch (const std::exception &e) {
    if (error) *error = std::string("inference failed: ") + e.what();
    spdlog::error("MNN LLM inference failed: {}", e.what());
    return resp;
  }

  resp.content = std::move(content);
  return resp;
}

std::vector<float> MnnInference::embed_text(const std::string &text,
                                            std::string *error) {
  std::lock_guard<std::mutex> global(global_mutex_);

  auto *emb = static_cast<MNN::Transformer::Embedding *>(ensure_embedding(error));
  if (!emb) return {};
  std::lock_guard<std::mutex> slot(emb_.mutex);

  try {
    MNN::Express::VARP out = emb->txt_embedding(text);
    auto info = out->getInfo();
    size_t n = 1;
    for (size_t i = 0; i < info->dim.size(); i++) n *= info->dim[i];
    const float *data = out->readMap<float>();
    return std::vector<float>(data, data + n);
  } catch (const std::exception &e) {
    if (error) *error = std::string("embedding failed: ") + e.what();
    spdlog::error("MNN embedding failed: {}", e.what());
    return {};
  }
}

bool MnnInference::load_llm(std::string *error) {
  std::lock_guard<std::mutex> global(global_mutex_);
  return ensure_llm(error) != nullptr;
}

bool MnnInference::load_embedding(std::string *error) {
  std::lock_guard<std::mutex> global(global_mutex_);
  return ensure_embedding(error) != nullptr;
}

#else  // !MC_HAVE_MNN

void *MnnInference::ensure_llm(std::string *error) {
  llm_.message = "MNN support not compiled (rebuild with -DMC_USE_MNN=ON)";
  if (error) *error = llm_.message;
  return nullptr;
}

void *MnnInference::ensure_embedding(std::string *error) {
  emb_.message = "MNN support not compiled (rebuild with -DMC_USE_MNN=ON)";
  if (error) *error = emb_.message;
  return nullptr;
}

LLMResponse MnnInference::chat(
    const std::vector<Message> &,
    const std::function<void(const AgentEvent&)> &, std::string *error) {
  llm_.message = "MNN support not compiled (rebuild with -DMC_USE_MNN=ON)";
  if (error) *error = llm_.message;
  return {};
}

std::vector<float> MnnInference::embed_text(const std::string &,
                                            std::string *error) {
  emb_.message = "MNN support not compiled (rebuild with -DMC_USE_MNN=ON)";
  if (error) *error = emb_.message;
  return {};
}

bool MnnInference::load_llm(std::string *error) {
  return ensure_llm(error) != nullptr;
}

bool MnnInference::load_embedding(std::string *error) {
  return ensure_embedding(error) != nullptr;
}

#endif  // MC_HAVE_MNN
