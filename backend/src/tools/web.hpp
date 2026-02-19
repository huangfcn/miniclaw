#pragma once
#include "tool.hpp"
#include <string>
#include <vector>
#include <regex>
#include <sstream>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <fiber.hpp>
#include "../agent/curl_manager.hpp"

using json = nlohmann::json;

// Helper to perform a fiber-blocking CURL request
inline std::string curl_fetch(const std::string& url, const std::vector<std::string>& headers = {}) {
    struct CurlData {
        std::string buffer;
        fiber_t fiber;
        std::function<void(CURLcode)> callback;
        struct curl_slist* header_list = nullptr;
    };

    auto* data = new CurlData{ "", fiber_ident(), nullptr, nullptr };
    data->callback = [data](CURLcode code) {
        fiber_resume(data->fiber);
    };

    CURL* easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PRIVATE, &data->callback);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);

    for (const auto& h : headers) {
        data->header_list = curl_slist_append(data->header_list, h.c_str());
    }
    if (data->header_list) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, data->header_list);
    }

    auto write_cb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        size_t total = size * nmemb;
        ((CurlData*)userdata)->buffer.append(ptr, total);
        return total;
    };
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, (curl_write_callback)write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, data);

    CurlMultiManager::instance().add_handle(easy);
    fiber_suspend(0);

    long response_code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);
    std::string result = (response_code == 200) ? data->buffer : "Error: HTTP " + std::to_string(response_code);

    CurlMultiManager::instance().remove_handle(easy);
    if (data->header_list) curl_slist_free_all(data->header_list);
    curl_easy_cleanup(easy);
    delete data;

    return result;
}

class WebSearchTool : public Tool {
public:
    WebSearchTool() {
        api_key_ = std::getenv("BRAVE_API_KEY") ? std::getenv("BRAVE_API_KEY") : "";
    }

    std::string name() const override { return "web_search"; }
    std::string description() const override { return "Search the web using Brave Search API"; }

    std::string execute(const std::string& input) override {
        if (api_key_.empty()) return "Error: BRAVE_API_KEY not configured";

        std::string query = input;
        char* encoded = curl_easy_escape(nullptr, query.c_str(), query.length());
        std::string url = "https://api.search.brave.com/res/v1/web/search?q=" + std::string(encoded);
        curl_free(encoded);

        std::string res = curl_fetch(url, {"X-Subscription-Token: " + api_key_});
        if (res.find("Error:") == 0) return res;

        try {
            auto j = json::parse(res);
            std::stringstream ss;
            ss << "Search results for: " << query << "\n\n";
            int count = 0;
            for (const auto& item : j["web"]["results"]) {
                ss << ++count << ". " << item["title"].get<std::string>() << "\n"
                   << "   URL: " << item["url"].get<std::string>() << "\n"
                   << "   " << item["description"].get<std::string>() << "\n\n";
                if (count >= 5) break;
            }
            return ss.str();
        } catch (...) {
            return "Error: Failed to parse search results";
        }
    }

private:
    std::string api_key_;
};

class WebFetchTool : public Tool {
public:
    std::string name() const override { return "web_fetch"; }
    std::string description() const override { return "Fetch a URL and extract text content"; }
    std::string execute(const std::string& input) override {
        std::string url = input;
        std::string res = curl_fetch(url);
        if (res.find("Error:") == 0) return res;

        return "URL: " + url + "\nContent (first 2000 chars):\n\n" + strip_html(res).substr(0, 2000);
    }

private:
    std::string strip_html(std::string html) {
        html = std::regex_replace(html, std::regex("<script[\\s\\S]*?</script>", std::regex::icase), "");
        html = std::regex_replace(html, std::regex("<style[\\s\\S]*?</style>", std::regex::icase), "");
        html = std::regex_replace(html, std::regex("<[^>]+>"), " ");
        html = std::regex_replace(html, std::regex("[ \\t]+"), " ");
        html = std::regex_replace(html, std::regex("\\n{22,}"), "\n\n");
        return html;
    }
};
