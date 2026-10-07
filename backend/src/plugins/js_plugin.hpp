#pragma once
// JS tool plugins.
//
// A plugin lives in <workspace>/plugins/<name>/ and consists of:
//    plugin.json — manifest: {name, version, description,
//                    tools:[{name, description, handler, parameters}]}
//    plugin.js   — JS source; for each tool it must define a global
//                    function `handler(args)` that returns a string.
//
// Inside plugin.js the global `api` object exposes the only host
// capabilities (see JsApi in js_engine.hpp):
//    api.fetch(url, {method, headers, body}) -> {status, body}
//    api.readFile(path) / api.writeFile(path, content)  (workspace-sandboxed)
//    api.config(section, key) — read a config value
//    api.log(msg) / api.now() / api.workspace()
//
// Plugin code has no other access to the host: no filesystem, no network,
// no process. It is interpreted by QuickJS (no JIT), which keeps it
// App Store / Play compliant on iOS and Android.

#include "js_engine.hpp"

#include <algorithm>
#include <ctime>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <simdjson.h>
#include <spdlog/spdlog.h>

#include "tools/tool.hpp"
#include "../agent/loop.hpp"
#include "../agent/curl_manager.hpp"
#include "../config.hpp"
#include <fiber.hpp>

namespace fs = std::filesystem;
namespace plugins {

// ─── Fiber-blocking HTTP (shared CurlMultiManager; same pattern as
//      curl_fetch in tools/web.hpp, with POST support + status) ──────────
inline JsFetchResult js_http_fetch(const std::string &url,
                                    const std::string &method,
                                    const std::vector<std::string> &headers,
                                    const std::string &body,
                                    long timeout_sec = 30) {
    struct HttpData {
         std::string buffer;
         fiber_t fiber;
         std::function<void(CURLcode)> callback;
         struct curl_slist *header_list = nullptr;
     };
     bool is_post = (method == "POST" || method == "PUT" || method == "PATCH");

     auto *data = new HttpData{"", fiber_ident(), nullptr, nullptr};
     data->callback = [data](CURLcode) { fiber_resume(data->fiber); };

     CURL *easy = curl_easy_init();
     curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
     if (is_post) {
       curl_easy_setopt(easy, CURLOPT_POST, 1L);
       curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body.c_str());
     }
     if (!method.empty() && !is_post && method != "GET")
       curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
     curl_easy_setopt(easy, CURLOPT_PRIVATE, &data->callback);
     curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
     curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
     curl_easy_setopt(easy, CURLOPT_TIMEOUT, timeout_sec);

     for (const auto &h : headers)
       data->header_list = curl_slist_append(data->header_list, h.c_str());
     if (data->header_list)
       curl_easy_setopt(easy, CURLOPT_HTTPHEADER, data->header_list);

     auto write_cb = [](char *ptr, size_t size, size_t nmemb, void *u) -> size_t {
       size_t total = size * nmemb;
       ((HttpData *)u)->buffer.append(ptr, total);
       return total;
     };
     curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
                      (curl_write_callback)write_cb);
     curl_easy_setopt(easy, CURLOPT_WRITEDATA, data);

     if (data->fiber) {
       // Fiber context (tool execution): block via the shared curl multi.
       CurlMultiManager::instance().add_handle(easy);
       fiber_suspend(0);
       CurlMultiManager::instance().remove_handle(easy);
     } else {
       // Non-fiber context (tests): synchronous request.
       curl_easy_perform(easy);
     }

     long response_code = 0;
     curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);

     JsFetchResult r;
     r.status = response_code;
     r.body = data->buffer;
     if (data->header_list) curl_slist_free_all(data->header_list);
     curl_easy_cleanup(easy);
     delete data;
     return r;
}

// ─── Workspace-sandboxed paths ────────────────────────────────────────────
inline std::string sandbox_path(const std::string &workspace,
                                const std::string &raw) {
    if (raw.empty()) return "";
    fs::path ws;
    try {
       ws = fs::canonical(workspace);
    } catch (...) {
       return "";
    }
    fs::path p = raw;
    if (!p.is_absolute()) p = ws / p;
    p = fs::weakly_canonical(p);
    if (p == ws) return "";
    if (!p.string().starts_with(ws.string() + "/"))
       return ""; // escape attempt
    return p.string();
}

// ─── Plugin manifest ─────────────────────────────────────────────────────
struct PluginToolSpec {
      std::string name;
      std::string description;
      std::string handler;          // global JS function name
      std::string parameters_json; // JSON object string (OpenAI schema)
};

struct PluginManifest {
      std::string name;
      std::string version;
      std::string description;
      std::string source; // full text of plugin.js
      std::vector<PluginToolSpec> tools;
};

// Escape a raw string as a JSON string literal.
inline std::string json_str_esc(std::string_view sv) {
    std::string out = "\"";
     for (char c : sv) {
          switch (c) {
           case '"': out += "\\\""; break;
           case '\\': out += "\\\\"; break;
           case '\n': out += "\\n"; break;
           case '\t': out += "\\t"; break;
           case '\r': out += "\\r"; break;
           default: out += c;
           }
      }
      return out + "\"";
}

// Serialize a simdjson DOM element to a JSON string (for re-embedding the
// manifest's `parameters` object into the tool schema).
inline std::string dom_to_json(const simdjson::dom::element &e) {
    if (e.is_object()) {
     simdjson::dom::object obj;
     if (e.get(obj)) return "{}";
       std::string out = "{";
       bool first = true;
       for (const auto &kv : obj) {
          if (!first) out += ",";
          first = false;
          out += json_str_esc(kv.key) + ":" + dom_to_json(kv.value);
        }
       return out + "}";
     }
     if (e.is_array()) {
       std::string out = "[";
       bool first = true;
       for (const auto &v : e) {
          if (!first) out += ",";
          first = false;
          out += dom_to_json(v);
        }
       return out + "]";
     }
     if (e.is_string()) {
       std::string_view sv;
       if (e.get(sv)) return "null";
       return json_str_esc(sv);
     }
     if (e.is_bool()) {
       bool b = false;
       if (e.get(b)) return "null";
       return b ? "true" : "false";
     }
     if (e.is_number()) {
       int64_t i;
       if (!e.get(i)) return std::to_string(i);
       double d;
       if (!e.get(d)) return std::to_string(d);
     }
     return "null";
}

// ─── JsPluginTool — a Tool backed by a JS handler ────────────────────────
class JsPluginTool : public Tool {
public:
      JsPluginTool(std::shared_ptr<PluginManifest> manifest,
                     PluginToolSpec spec, std::string workspace,
                     std::string plugin_name)
            : manifest_(std::move(manifest)), spec_(std::move(spec)),
             workspace_(std::move(workspace)),
             plugin_name_(std::move(plugin_name)) {}

      std::string name() const override { return spec_.name; }
      std::string description() const override { return spec_.description; }

      std::string schema() const override {
          std::string params = spec_.parameters_json.empty()
                                    ? "{\"type\":\"object\",\"properties\":{}}"
                                    : spec_.parameters_json;
          return "{\"type\":\"function\",\"function\":{\"name\":\"" +
                    json_str_esc(spec_.name) +
                    "\",\"description\":\"" + json_str_esc(spec_.description) +
                    "\",\"parameters\":" + params + "}}";
       }

      std::string execute(const std::string &) override {
          return "Error: tool '" + spec_.name +
                    "' requires named arguments (see its schema)";
       }

      std::string execute(
            const std::map<std::string, std::string> &args) override {
          JsApi api;
          api.fetch = [](const std::string &url,
                            const std::string &method,
                            const std::vector<std::string> &headers,
                            const std::string &body) {
             return js_http_fetch(url, method, headers, body);
           };
           api.read_file = [ws = workspace_](const std::string &path) {
             std::string full = sandbox_path(ws, path);
             if (full.empty()) return std::string{};
             std::ifstream in(full);
             if (!in) return std::string{};
             std::stringstream ss;
             ss << in.rdbuf();
             return ss.str();
           };
           api.write_file = [ws = workspace_](const std::string &path,
                                                const std::string &content) {
              std::string full = sandbox_path(ws, path);
              if (full.empty()) return false;
              try {
                fs::create_directories(fs::path(full).parent_path());
                std::ofstream out(full);
                if (!out) return false;
                out << content;
                return true;
              } catch (const std::exception &ex) {
                spdlog::warn("plugin writeFile failed for {}: {}", full,
                               ex.what());
                return false;
              }
           };
           api.config = [](const std::string &section,
                              const std::string &key) {
              return Config::instance().get_string(section, key);
           };
           api.log = [this](const std::string &msg) {
              spdlog::info("[plugin:{}] {}", plugin_name_, msg);
           };
           api.now = []() {
              std::time_t t = std::time(nullptr);
              std::tm tm{};
#if defined(_WIN32)
              localtime_s(&tm, &t); // MSVC/MinGW: (tm*, time_t*) — reversed args
#else
              localtime_r(&t, &tm);
#endif
              char buf[32];
              std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
              return std::string(buf);
           };
           api.workspace = workspace_;

           auto r = JsEngine::instance().call(manifest_->source, spec_.handler,
                                                args, api);
           if (r.ok)
             return r.value;
           spdlog::warn("plugin tool '{}' failed: {}", spec_.name, r.error);
           return "Error: " + r.error;
       }

private:
      std::shared_ptr<PluginManifest> manifest_;
      PluginToolSpec spec_;
      std::string workspace_;
      std::string plugin_name_ = "js";
};

// ─── PluginLoader — scans <workspace>/plugins/*/plugin.json ──────────────
class PluginLoader {
public:
      explicit PluginLoader(const std::string &workspace)
            : workspace_(workspace) {}

      // Load all plugins; one JsPluginTool per declared tool.
      std::vector<std::shared_ptr<Tool>> load() {
          std::vector<std::shared_ptr<Tool>> out;
          fs::path dir = fs::path(workspace_) / "plugins";
          if (!fs::exists(dir) || !fs::is_directory(dir)) return out;

          std::vector<fs::path> entries;
          for (const auto &e : fs::directory_iterator(dir))
             if (e.is_directory()) entries.push_back(e.path());
          std::sort(entries.begin(), entries.end());

          for (const auto &p : entries) {
             fs::path json_path = p / "plugin.json";
             if (!fs::exists(json_path)) continue;
             auto manifest = parse_plugin(p, json_path);
             if (!manifest) continue;
             for (const auto &spec : manifest->tools) {
               auto tool = std::make_shared<JsPluginTool>(
                     manifest, spec, workspace_, manifest->name);
               spdlog::info("Loaded JS plugin tool: {} (plugin {} v{})",
                              tool->name(), manifest->name,
                              manifest->version);
               out.push_back(std::move(tool));
             }
           }
          return out;
       }

      void register_all(AgentLoop &loop) {
          for (auto &t : load()) {
              // capture name before moving t (unspecified arg eval order)
              std::string name = t->name();
              loop.register_tool(name, std::move(t));
          }
       }

private:
      std::shared_ptr<PluginManifest> parse_plugin(
            const fs::path &plugin_dir, const fs::path &json_path) {
          std::ifstream in(json_path);
          if (!in) return nullptr;
          std::stringstream ss;
          ss << in.rdbuf();
          std::string text = ss.str();

          auto manifest = std::make_shared<PluginManifest>();
          try {
             simdjson::dom::parser parser;
             simdjson::dom::element root;
             auto padded = simdjson::padded_string(text);
             if (parser.parse(padded).get(root)) {
               spdlog::warn("plugin {}: invalid plugin.json",
                              plugin_dir.filename().string());
               return nullptr;
             }
             std::string_view name;
             if (root["name"].get(name) || name.empty()) {
               spdlog::warn("plugin {}: missing name",
                              plugin_dir.filename().string());
               return nullptr;
             }
             manifest->name = std::string(name);
             std::string_view sv;
             if (!root["version"].get(sv))
               manifest->version = std::string(sv);
             if (!root["description"].get(sv))
               manifest->description = std::string(sv);

             // JS source: explicit `file` field or default plugin.js
             fs::path js;
             if (!root["file"].get(sv))
               js = plugin_dir / std::string(sv);
             else
               js = plugin_dir / "plugin.js";
             if (!fs::exists(js)) {
               spdlog::warn("plugin {}: {} not found",
                              plugin_dir.filename().string(), js.filename().string());
               return nullptr;
             }
             std::ifstream jf(js);
             std::stringstream jss;
             jss << jf.rdbuf();
             manifest->source = jss.str();

             simdjson::dom::array tools;
             if (root["tools"].get(tools)) {
               spdlog::warn("plugin {}: no tools declared",
                              plugin_dir.filename().string());
               return nullptr;
             }
             for (const auto &t : tools) {
               PluginToolSpec spec;
               std::string_view s2;
               if (t["name"].get(s2) || s2.empty()) continue;
               spec.name = std::string(s2);
               if (!t["description"].get(s2))
                 spec.description = std::string(s2);
               if (!t["handler"].get(s2) && !s2.empty())
                 spec.handler = std::string(s2);
               else if (spec.handler.empty())
                 spec.handler = spec.name; // default: handler == tool name
               simdjson::dom::element params;
               if (!t["parameters"].get(params))
                 spec.parameters_json = dom_to_json(params);
               manifest->tools.push_back(std::move(spec));
             }
           } catch (const std::exception &ex) {
             spdlog::warn("plugin {}: parse error: {}",
                            plugin_dir.filename().string(), ex.what());
             return nullptr;
           }
          if (manifest->tools.empty()) {
             spdlog::warn("plugin {}: no valid tools",
                            plugin_dir.filename().string());
             return nullptr;
           }
          return manifest;
       }

      std::string workspace_;
};

} // namespace plugins
