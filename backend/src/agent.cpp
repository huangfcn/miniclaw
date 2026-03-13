#include "agent.hpp"
#include "json_util.hpp"
#include <cmath>
#include <condition_variable>
#include <curl/curl.h>
#include <mutex>
#include <queue>
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <uv.h>


#include "agent/subagent.hpp"
#include "tools/busybox.hpp"
#include "tools/file.hpp"
#include "tools/gmail.hpp"
#include "tools/spawn.hpp"
#include "tools/terminal.hpp"
#include "tools/web.hpp"


static thread_local std::string t_session_id;

void init_spawn_system() {
  curl_global_init(CURL_GLOBAL_ALL);
  spdlog::info("Spawn system ready");
}

void spawn_in_pool(std::function<void()> task) {
  FiberPool::instance().spawn(std::move(task));
}

std::string Agent::current_session_id() {
  return t_session_id;
}

Agent::~Agent() = default;

Agent::Agent() {
  api_key_ = Config::instance().conversation_api_key();
  api_base_ = Config::instance().conversation_endpoint();
  model_ = Config::instance().conversation_model();
  workspace_ = Config::instance().memory_workspace();

  // Build the LLM call function (now takes model, endpoint and provider too)
  LLMCallFn llm_fn =
      [this](const std::vector<Message> &messages,
             const std::string &tools_json, AgentEventCallback on_event,
             const std::string &model, const std::string &endpoint,
             const std::string &provider) -> LLMResponse {
    return call_llm(messages, tools_json, on_event, model, endpoint, provider);
  };

  EmbeddingFn embed_fn = [this](const std::string &text) -> std::vector<float> {
    return embed(text);
  };

  loop_ = std::make_unique<AgentLoop>(workspace_, llm_fn, embed_fn,
                                      /*max_iterations=*/10);
  sessions_ = std::make_unique<SessionManager>(workspace_);
  subagents_ = std::make_unique<SubagentManager>(workspace_, llm_fn, embed_fn);

  // Register built-in tools
  loop_->register_tool("exec", std::make_shared<TerminalTool>());
  loop_->register_tool("read_file", std::make_shared<ReadFileTool>());
  loop_->register_tool("write_file", std::make_shared<WriteFileTool>());
  loop_->register_tool("edit_file", std::make_shared<EditFileTool>());
  loop_->register_tool("list_dir", std::make_shared<ListDirTool>());
  loop_->register_tool("web_search", std::make_shared<WebSearchTool>());
  loop_->register_tool("web_fetch", std::make_shared<WebFetchTool>());
  loop_->register_tool("spawn", std::make_shared<SpawnTool>(*subagents_));
  loop_->register_tool("gmail", std::make_shared<GmailTool>());

#ifdef _WIN32
#endif

  // Register memory search tool schema (handled in AgentLoop::process)
  struct MemorySearchTool : public Tool {
    std::string name() const override { return "memory_search"; }
    std::string description() const override {
      return "Search through the agent's multi-tiered memory (sessions, daily "
             "logs, curated facts) using hybrid search (vector + keyword).";
    }
    std::string schema() const override {
      return "{\"type\":\"function\",\"function\":{\"name\":\"memory_search\","
             "\"description\":\"Search through "
             "memory.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
             "\"query\":{\"type\":\"string\",\"description\":\"The search "
             "query.\"}},\"required\":[\"query\"]}}}";
    }
    std::string execute(const std::map<std::string, std::string> &) override {
      return "";
    } // Handled in loop
  };
  loop_->register_tool("memory_search", std::make_shared<MemorySearchTool>());

  struct IndexDocumentTool : public Tool {
    std::string name() const override { return "index_document"; }
    std::string description() const override {
      return "Index a document (file or text snippet) into the agent's hybrid "
             "memory for future retrieval.";
    }
    std::string schema() const override {
      return "{\"type\":\"function\",\"function\":{\"name\":\"index_document\","
             "\"description\":\"Index a document.\",\"parameters\":{\"type\":"
             "\"object\",\"properties\":{\"path\":{\"type\":\"string\","
             "\"description\":\"A unique identifier or file path.\"},\"text\":"
             "{\"type\":\"string\",\"description\":\"The content to index.\"}},"
             "\"required\":[\"path\",\"text\"]}}}";
    }
    std::string execute(const std::map<std::string, std::string> &) override {
      return "";
    } // Handled in loop
  };
  loop_->register_tool("index_document", std::make_shared<IndexDocumentTool>());

  spdlog::info("Agent initialized: model={} workspace={}", model_, workspace_);
}

void Agent::run(const std::string &user_message, const std::string &session_id,
                AgentEventCallback on_event, const std::string &channel) {
  auto session = sessions_->get_or_create(session_id);
  t_session_id = session_id;

  try {
    loop_->process(user_message, session, on_event, channel, session_id);
  } catch (const std::exception &e) {
    spdlog::error("Error in Agent::run: {}", e.what());
    on_event({"error", e.what()});
  }

  t_session_id = "";
  sessions_->save(session);
}

// ─── Serialize a Message vector to JSON ──────────────────────────────────────

static std::string serialize_messages(const std::vector<Message> &messages) {
  std::string j = "[";
  for (size_t i = 0; i < messages.size(); ++i) {
    if (i > 0)
      j += ",";
    const auto &m = messages[i];

    j += "{\"role\":\"" + json_util::escape(m.role) + "\"";

    if (m.role == "assistant" && !m.tool_calls_json.empty()) {
      // Assistant message with tool_calls (may also have text content)
      if (!m.content.empty()) {
        j += ",\"content\":\"" + json_util::escape(m.content) + "\"";
      } else {
        j += ",\"content\":null";
      }
      j += ",\"tool_calls\":" + m.tool_calls_json;
    } else if (m.role == "tool") {
      // Tool result message
      j += ",\"tool_call_id\":\"" + json_util::escape(m.tool_call_id) + "\"";
      j += ",\"name\":\"" + json_util::escape(m.name) + "\"";
      j += ",\"content\":\"" + json_util::escape(m.content) + "\"";
    } else {
      j += ",\"content\":\"" + json_util::escape(m.content) + "\"";
    }

    j += "}";
  }
  j += "]";
  return j;
}

// ─── Accumulator for streaming tool_calls ────────────────────────────────────

struct ToolCallAccum {
  std::string id;
  std::string name;
  std::string arguments; // accumulated incrementally
};

// ─── call_llm: sends messages + tools, parses streaming tool_calls ───────────

std::vector<float> Agent::embed(const std::string &text) {
  std::string provider = Config::instance().embedding_provider();
  std::string model = Config::instance().embedding_model();
  std::string endpoint = Config::instance().embedding_endpoint();

  spdlog::debug("Embedding using provider: {}, model: {}, endpoint: {}",
                provider, model, endpoint);

  std::string buffer;
  std::vector<float> embedding;
  struct curl_slist *headers = nullptr;

  CURL *easy = curl_easy_init();

  std::string payload = "{\"model\":\"" + model + "\",\"input\":\"" +
                        json_util::escape(text) + "\"}";

  headers = curl_slist_append(headers, "Content-Type: application/json");

  std::string effective_key = api_key_;
  if (provider == "openai" || !api_key_.empty()) {
    std::string auth = "Authorization: Bearer " + effective_key;
    headers = curl_slist_append(headers, auth.c_str());
  }

  std::string effective_endpoint = endpoint;
  if (effective_endpoint.empty()) {
    if (api_base_.find("http") == 0) {
      effective_endpoint = api_base_;
      if (effective_endpoint.find("/v1/chat/completions") == std::string::npos &&
          effective_endpoint.back() != '/') {
        effective_endpoint += "/v1/chat/completions";
      }
    } else {
      effective_endpoint = "https://" + api_base_ + "/v1/chat/completions";
    }
  }

  curl_easy_setopt(easy, CURLOPT_URL, effective_endpoint.c_str());
  curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, payload.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

  auto write_cb = [](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
    ((std::string *)userdata)->append(ptr, size * nmemb);
    return size * nmemb;
  };
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, (curl_write_callback)write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buffer);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);

  CURLcode res = curl_easy_perform(easy);
  if (res != CURLE_OK) {
      spdlog::error("CURL perform failed: {}", curl_easy_strerror(res));
  }

  try {
    simdjson::dom::parser parser;
    simdjson::dom::element j;
    auto padded = simdjson::padded_string(buffer);
    if (!parser.parse(padded).get(j)) {
      simdjson::dom::array data_arr;
      if (!j["data"].get(data_arr) && data_arr.size() > 0) {
        simdjson::dom::array emb_arr;
        if (!data_arr.at(0)["embedding"].get(emb_arr)) {
          for (auto val : emb_arr) {
            double d;
            if (!val.get(d)) embedding.push_back((float)d);
          }
        }
      }
    }
  } catch (...) {}

  int target_dim = Config::instance().embedding_dimension();
  if (embedding.size() > (size_t)target_dim) {
    embedding.resize(target_dim);
    double sum_sq = 0;
    for (float v : embedding) sum_sq += (double)v * v;
    float norm = (float)std::sqrt(sum_sq);
    if (norm > 1e-9f) {
      for (float &v : embedding) v /= norm;
    }
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return embedding;
}

LLMResponse Agent::call_llm(const std::vector<Message> &messages,
                            const std::string &tools_json,
                            AgentEventCallback on_event,
                            const std::string &model,
                            const std::string &endpoint,
                            const std::string &provider) {
  struct ToolCallAccum {
    std::string id, name, arguments;
  };
  struct CallData {
    AgentEventCallback on_event;
    std::string text_content;
    std::string buffer;
    simdjson::dom::parser parser;
    std::vector<ToolCallAccum> tool_calls;
  };

  CallData data;
  data.on_event = on_event;
  CURL *easy = curl_easy_init();

  std::string j_messages = serialize_messages(messages);
  std::string effective_model = model.empty() ? model_ : model;
  std::string payload_str = "{\"model\":\"" + effective_model + "\",\"messages\":" + j_messages + ",\"stream\":true";

  if (!tools_json.empty() && tools_json != "[]") {
    payload_str += ",\"tools\":" + tools_json + ",\"tool_choice\":\"auto\"";
  }
  payload_str += "}";

  spdlog::debug("LLM payload (first 1000 chars): {}",
                payload_str.substr(0, 1000));

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!api_key_.empty()) {
    std::string auth = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth.c_str());
  }

  std::string effective_endpoint = endpoint;
  if (effective_endpoint.empty()) {
    std::string url = api_base_;
    if (url.find("http") != 0) url = "https://" + url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() < 3 || url.substr(url.size() - 3) != "/v1") url += "/v1";
    url += "/chat/completions";
    effective_endpoint = url;
  }
  spdlog::info("LLM URL: {} Model: {}", effective_endpoint, effective_model);

  curl_easy_setopt(easy, CURLOPT_URL, effective_endpoint.c_str());
  curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, payload_str.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

  auto write_cb = [](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
    size_t total = size * nmemb;
    auto *d = (CallData *)userdata;
    d->buffer.append(ptr, total);

    size_t pos;
    while ((pos = d->buffer.find('\n')) != std::string::npos) {
      std::string line = d->buffer.substr(0, pos);
      d->buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();

      if (line.rfind("data: ", 0) != 0) {
        if (!line.empty()) {
          spdlog::debug("LLM raw line: {}", line);
          simdjson::dom::element j;
          auto padded = simdjson::padded_string(line);
          if (!d->parser.parse(padded).get(j)) {
            simdjson::dom::element err;
            if (!j["error"].get(err)) {
              std::string_view msg;
              if (!err["message"].get(msg))
                d->text_content = "Error: " + std::string(msg);
              else
                d->text_content = "Error: " + simdjson::to_string(err);
            }
          }
        }
        continue;
      }
      std::string json_str = line.substr(6);
      if (json_str == "[DONE]" || json_str.empty()) continue;

      simdjson::dom::element j;
      auto padded = simdjson::padded_string(json_str);
      auto error = d->parser.parse(padded).get(j);
      if (error) {
        spdlog::warn("simdjson parse error in write_cb: {}",
                     json_str.substr(0, 200));
        continue;
      }

      simdjson::dom::array choices;
      if (j["choices"].get(choices) || choices.size() == 0) continue;

      simdjson::dom::element delta;
      if (choices.at(0)["delta"].get(delta)) continue;

      std::string_view content_sv;
      if (!delta["content"].get(content_sv) && !content_sv.empty()) {
        std::string tok(content_sv);
        d->on_event({"token", tok});
        d->text_content += tok;
      }

      simdjson::dom::array tc_arr;
      if (!delta["tool_calls"].get(tc_arr)) {
        for (auto tc_elem : tc_arr) {
          int64_t idx = 0;
          if (tc_elem["index"].get(idx)) {
            spdlog::warn("Missing index in tool_call chunk");
          }
          while ((int64_t)d->tool_calls.size() <= idx) d->tool_calls.push_back({});
          auto &accum = d->tool_calls[idx];
          std::string_view id_sv;
          if (!tc_elem["id"].get(id_sv)) accum.id = std::string(id_sv);
          simdjson::dom::element fn_elem;
          if (!tc_elem["function"].get(fn_elem)) {
            std::string_view name_sv, args_sv;
            if (!fn_elem["name"].get(name_sv) && !name_sv.empty()) accum.name = std::string(name_sv);
            if (!fn_elem["arguments"].get(args_sv)) accum.arguments += std::string(args_sv);
          }
        }
      }
    }
    return total;
  };

  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, (curl_write_callback)write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &data);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 300L);

  CURLcode res = curl_easy_perform(easy);
  if (res != CURLE_OK) {
    spdlog::error("CURL perform failed: {}", curl_easy_strerror(res));
  } else {
    long http_code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
      if (data.text_content.empty()) {
        data.text_content = "Error: LLM API returned HTTP " + std::to_string(http_code);
      }
    }
  }

  // Final buffer check if anything remains
  if (!data.buffer.empty()) {
    // Process last line if it doesn't end in newline
  }

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(easy);

  LLMResponse response;
  response.content = data.text_content;
  for (const auto &accum : data.tool_calls) {
    ToolCall tc;
    tc.id = accum.id.empty() ? ("call_" + std::to_string(rand())) : accum.id;
    tc.name = accum.name;
    tc.arguments_json = accum.arguments.empty() ? "{}" : accum.arguments;
    response.tool_calls.push_back(tc);
  }

  return response;
}
