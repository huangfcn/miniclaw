#pragma once
#include "../agent/curl_manager.hpp"
#include "../config.hpp"
#include "../json_util.hpp"
#include "tool.hpp"
#include <curl/curl.h>
#include <fiber.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class GmailTool : public Tool {
public:
  GmailTool() {
    workspace_ = Config::instance().memory_workspace();
    skills_dir_ = fs::path(workspace_) / "skills" / "gmail";
  }

  std::string name() const override { return "gmail"; }

  std::string description() const override {
    return "Interact with Gmail to list unread emails, fetch message content, "
           "and authenticate.";
  }

  std::string schema() const override {
    return R"===({
            "type": "function",
            "function": {
                "name": "gmail",
                "description": "Gmail interaction tool",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "action": {
                            "type": "string",
                            "enum": ["list", "get", "check_creds", "auth"],
                            "description": "Action to perform"
                        },
                        "query": {
                            "type": "string",
                            "description": "Search query for 'list' action (e.g. 'is:unread')"
                        },
                        "message_id": {
                            "type": "string",
                            "description": "Message ID for 'get' action"
                        },
                        "auth_code": {
                            "type": "string",
                            "description": "Authorization code for 'auth' action"
                        }
                    },
                    "required": ["action"]
                }
            }
        })===";
  }

  std::string execute(const std::map<std::string, std::string> &args) override {
    auto it = args.find("action");
    if (it == args.end())
      return "Error: missing 'action' argument";
    std::string action = it->second;

    if (action == "check_creds") {
      return check_creds();
    } else if (action == "auth") {
      auto code_it = args.find("auth_code");
      std::string code = (code_it != args.end()) ? code_it->second : "";
      return auth(code);
    } else if (action == "list") {
      auto q_it = args.find("query");
      std::string query = (q_it != args.end()) ? q_it->second : "is:unread";
      return list_messages(query);
    } else if (action == "get") {
      auto id_it = args.find("message_id");
      if (id_it == args.end())
        return "Error: missing 'message_id' for get action";
      return get_message(id_it->second);
    }

    return "Error: unknown action " + action;
  }

private:
  std::string workspace_;
  fs::path skills_dir_;

  struct Token {
    std::string access_token;
    std::string refresh_token;
    long long expires_at = 0;
  };

  struct Credentials {
    std::string client_id;
    std::string client_secret;
    std::string auth_uri;
    std::string token_uri;
    std::string redirect_uri;
  };

  std::string check_creds() {
    fs::path creds_path = skills_dir_ / "credentials.json";
    if (!fs::exists(creds_path)) {
      creds_path = skills_dir_ / "credentials_huangfcn.json";
    }
    if (!fs::exists(creds_path)) {
      return "Error: No Gmail credentials.json found in skills/gmail/";
    }
    return "Gmail credentials found and valid.";
  }

  Credentials load_credentials() {
    fs::path creds_path = skills_dir_ / "credentials.json";
    if (!fs::exists(creds_path)) {
      creds_path = skills_dir_ / "credentials_huangfcn.json";
    }
    Credentials c;
    if (!fs::exists(creds_path))
      return c;

    try {
      std::ifstream f(creds_path);
      std::stringstream ss;
      ss << f.rdbuf();
      std::string content = ss.str();

      simdjson::dom::parser parser;
      simdjson::dom::element j;
      auto error = parser.parse(content).get(j);
      if (!error) {
        simdjson::dom::element root;
        if (!j["installed"].get(root) || !j["web"].get(root)) {
          std::string_view cid, cs, auri, turi;
          if (!root["client_id"].get(cid))
            c.client_id = std::string(cid);
          if (!root["client_secret"].get(cs))
            c.client_secret = std::string(cs);
          if (!root["auth_uri"].get(auri))
            c.auth_uri = std::string(auri);
          if (!root["token_uri"].get(turi))
            c.token_uri = std::string(turi);

          simdjson::dom::array redirects;
          if (!root["redirect_uris"].get(redirects) && redirects.size() > 0) {
            std::string_view ruri;
            if (!redirects.at(0).get(ruri))
              c.redirect_uri = std::string(ruri);
          }

          if (c.auth_uri.empty())
            c.auth_uri = "https://accounts.google.com/o/oauth2/auth";
          if (c.token_uri.empty())
            c.token_uri = "https://oauth2.googleapis.com/token";
          if (c.redirect_uri.empty())
            c.redirect_uri = "http://localhost";
        }
      }
    } catch (...) {
    }
    return c;
  }

  Token load_token() {
    fs::path token_path = skills_dir_ / "token.json";
    Token t;
    if (!fs::exists(token_path))
      return t;

    try {
      std::ifstream f(token_path);
      std::stringstream ss;
      ss << f.rdbuf();
      std::string content = ss.str();

      simdjson::dom::parser parser;
      simdjson::dom::element j;
      auto error = parser.parse(content).get(j);
      if (!error) {
        std::string_view at, rt;
        if (!j["access_token"].get(at))
          t.access_token = std::string(at);
        if (!j["refresh_token"].get(rt))
          t.refresh_token = std::string(rt);
      }
    } catch (...) {
    }
    return t;
  }

  void save_token(const Token &t) {
    fs::path token_path = skills_dir_ / "token.json";
    std::string json = "{";
    json += "\"access_token\":\"" + json_util::escape(t.access_token) + "\",";
    json += "\"refresh_token\":\"" + json_util::escape(t.refresh_token) + "\",";
    json += "\"token_type\":\"Bearer\"";
    json += "}";

    std::ofstream f(token_path);
    f << json;
  }

  std::string auth(const std::string &code) {
    Credentials creds = load_credentials();
    if (creds.client_id.empty())
      return "Error: Failed to load credentials.";

    if (code.empty()) {
      // Generate Auth URL
      std::string url =
          creds.auth_uri + "?client_id=" + creds.client_id +
          "&redirect_uri=" + creds.redirect_uri + "&response_type=code" +
          "&scope=https://www.googleapis.com/auth/gmail.readonly" +
          "&access_type=offline" + "&prompt=consent";

      return "Please visit this URL to authorize Gmail access, then call "
             "gmail(action='auth', auth_code='...') with the code you "
             "received:\n\n" +
             url;
    }

    // Exchange code for tokens
    std::string post_fields = "client_id=" + creds.client_id +
                              "&client_secret=" + creds.client_secret +
                              "&code=" + code +
                              "&redirect_uri=" + creds.redirect_uri +
                              "&grant_type=authorization_code";

    std::string res = request(creds.token_uri, "POST", {}, post_fields);
    if (res.find("Error:") == 0)
      return res;

    try {
      simdjson::dom::parser parser;
      simdjson::dom::element j;
      auto error = parser.parse(res).get(j);
      if (!error) {
        Token t;
        std::string_view at, rt;
        if (!j["access_token"].get(at))
          t.access_token = std::string(at);
        if (!j["refresh_token"].get(rt))
          t.refresh_token = std::string(rt);

        if (!t.access_token.empty()) {
          save_token(t);
          return "Successfully authenticated! tokens saved.";
        }
      }
    } catch (...) {
    }

    return "Error: Failed to parse token response: " + res;
  }

  std::string try_refresh(Token &token) {
    if (token.refresh_token.empty())
      return "Error: No refresh token available.";

    Credentials creds = load_credentials();
    if (creds.client_id.empty() || creds.client_secret.empty())
      return "Error: Missing credentials for refresh.";

    std::string post_fields = "client_id=" + creds.client_id +
                              "&client_secret=" + creds.client_secret +
                              "&refresh_token=" + token.refresh_token +
                              "&grant_type=refresh_token";

    std::string res = request(creds.token_uri, "POST", {}, post_fields);
    if (res.find("Error:") == 0)
      return res;

    try {
      simdjson::dom::parser parser;
      simdjson::dom::element j;
      auto error = parser.parse(res).get(j);
      if (!error) {
        std::string_view at;
        if (!j["access_token"].get(at)) {
          token.access_token = std::string(at);
          save_token(token);
          return "Success";
        }
      }
    } catch (...) {
    }

    return "Error: Failed to parse refresh token response: " + res;
  }

  std::string list_messages(const std::string &query) {
    Token token = load_token();
    if (token.access_token.empty())
      return "Error: Not authenticated. Use gmail(action='auth') to authorize.";

    char *encoded_q = curl_easy_escape(nullptr, query.c_str(), query.length());
    std::string url =
        "https://gmail.googleapis.com/gmail/v1/users/me/messages?q=" +
        std::string(encoded_q);
    curl_free(encoded_q);

    for (int retry = 0; retry < 2; ++retry) {
      std::string res =
          request(url, "GET", {"Authorization: Bearer " + token.access_token});
      if (res.find("Error: HTTP 401") != std::string::npos) {
        spdlog::info("Access token expired, attempting refresh...");
        if (try_refresh(token) == "Success")
          continue;
      }
      return res;
    }
    return "Error: Failed after refresh attempt.";
  }

  std::string get_message(const std::string &id) {
    Token token = load_token();
    if (token.access_token.empty())
      return "Error: Not authenticated.";

    std::string url =
        "https://gmail.googleapis.com/gmail/v1/users/me/messages/" + id +
        "?format=full";

    for (int retry = 0; retry < 2; ++retry) {
      std::string res =
          request(url, "GET", {"Authorization: Bearer " + token.access_token});
      if (res.find("Error: HTTP 401") != std::string::npos) {
        spdlog::info("Access token expired, attempting refresh...");
        if (try_refresh(token) == "Success")
          continue;
      }
      return res;
    }
    return "Error: Failed after refresh attempt.";
  }

  std::string request(const std::string &url, const std::string &method,
                      const std::vector<std::string> &headers,
                      const std::string &post_data = "") {
    struct CurlData {
      std::string buffer;
      fiber_t fiber;
      std::function<void(CURLcode)> callback;
      struct curl_slist *header_list = nullptr;
    };

    auto *data = new CurlData{"", fiber_ident(), nullptr, nullptr};
    data->callback = [data](CURLcode code) { fiber_resume(data->fiber); };

    CURL *easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PRIVATE, &data->callback);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);

    if (method == "POST") {
      curl_easy_setopt(easy, CURLOPT_POST, 1L);
      if (!post_data.empty()) {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, post_data.c_str());
      }
    }

    for (const auto &h : headers) {
      data->header_list = curl_slist_append(data->header_list, h.c_str());
    }
    if (data->header_list) {
      curl_easy_setopt(easy, CURLOPT_HTTPHEADER, data->header_list);
    }

    auto write_cb = [](char *ptr, size_t size, size_t nmemb,
                       void *userdata) -> size_t {
      size_t total = size * nmemb;
      ((CurlData *)userdata)->buffer.append(ptr, total);
      return total;
    };
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
                     (curl_write_callback)write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, data);

    CurlMultiManager::instance().add_handle(easy);
    fiber_suspend(0);

    long response_code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);
    std::string result = (response_code >= 200 && response_code < 300)
                             ? data->buffer
                             : "Error: HTTP " + std::to_string(response_code) +
                                   "\n" + data->buffer;

    CurlMultiManager::instance().remove_handle(easy);
    if (data->header_list)
      curl_slist_free_all(data->header_list);
    curl_easy_cleanup(easy);
    delete data;

    return result;
  }
};
