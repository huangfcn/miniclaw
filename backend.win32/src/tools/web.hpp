#pragma once
// WebSearchTool + WebFetchTool — with native function-calling schema

#include "tool.hpp"
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <sstream>
#include <simdjson.h>
#include <curl/curl.h>

// Helper to perform a blocking CURL request
inline std::string curl_fetch(const std::string& url, const std::vector<std::string>& headers = {}) {
    std::string buffer;
    struct curl_slist* header_list = nullptr;

    CURL* easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);

    for (const auto& h : headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, header_list);
    }

    auto write_cb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        ((std::string*)userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, (curl_write_callback)write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buffer);

    CURLcode res = curl_easy_perform(easy);
    
    long response_code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);
    
    std::string result;
    if (res == CURLE_OK && response_code == 200) {
        result = buffer;
    } else {
        result = "Error: HTTP " + std::to_string(response_code) + (res != CURLE_OK ? " (" + std::string(curl_easy_strerror(res)) + ")" : "");
    }

    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(easy);

    return result;
}

class WebSearchTool : public Tool {
public:
    WebSearchTool() {
        api_key_ = std::getenv("BRAVE_API_KEY") ? std::getenv("BRAVE_API_KEY") : "";
    }

    std::string name() const override { return "web_search"; }
    std::string description() const override {
        return "Search the web using Brave Search API. Returns titles, URLs, and snippets.";
    }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"web_search","description":"Search the web using Brave Search API. Returns titles, URLs, and snippets.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"The search query"},"count":{"type":"integer","description":"Number of results (default 5)"}},"required":["query"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it = args.find("query");
        if (it == args.end()) return "Error: missing 'query' argument";
        return execute(it->second);
    }

    std::string execute(const std::string& input) override {
        if (api_key_.empty()) return "Error: BRAVE_API_KEY not configured";

        char* encoded = curl_easy_escape(nullptr, input.c_str(), input.length());
        std::string url = "https://api.search.brave.com/res/v1/web/search?q=" + std::string(encoded);
        curl_free(encoded);

        std::string res = curl_fetch(url, {"X-Subscription-Token: " + api_key_});
        if (res.find("Error:") == 0) return res;

        try {
            simdjson::dom::parser parser;
            simdjson::dom::element j;
            auto error = parser.parse(res).get(j);
            if (error) return "Error: Failed to parse search results";

            std::stringstream ss;
            ss << "Search results for: " << input << "\n\n";
            int count = 0;
            simdjson::dom::array results;
            if (!j["web"]["results"].get(results)) {
                for (auto item : results) {
                    std::string_view title_sv, url_sv, desc_sv;
                    if (!item["title"].get(title_sv) &&
                        !item["url"].get(url_sv) &&
                        !item["description"].get(desc_sv)) {
                        
                        ss << ++count << ". " << title_sv << "\n"
                           << "   URL: " << url_sv << "\n"
                           << "   " << desc_sv << "\n\n";
                    }
                    if (count >= 5) break;
                }
            }
            return ss.str();
        } catch (...) {
            return "Error: Exception during parsing search results";
        }
    }

private:
    std::string api_key_;
};

class WebFetchTool : public Tool {
public:
    std::string name() const override { return "web_fetch"; }
    std::string description() const override { return "Fetch a URL and return its text content."; }

    std::string schema() const override {
        return R"===({"type":"function","function":{"name":"web_fetch","description":"Fetch a URL and return its text content.","parameters":{"type":"object","properties":{"url":{"type":"string","description":"The URL to fetch"}},"required":["url"]}}})===";
    }

    std::string execute(const std::map<std::string, std::string>& args) override {
        auto it = args.find("url");
        if (it == args.end()) return "Error: missing 'url' argument";
        return execute(it->second);
    }

    std::string execute(const std::string& input) override {
        std::string res = curl_fetch(input);
        if (res.find("Error:") == 0) return res;
        return "URL: " + input + "\nContent (first 3000 chars):\n\n" + strip_html(res).substr(0, 3000);
    }

private:
    std::string strip_html(std::string html) {
        html = std::regex_replace(html, std::regex("<script[\\s\\S]*?</script>", std::regex::icase), "");
        html = std::regex_replace(html, std::regex("<style[\\s\\S]*?</style>", std::regex::icase), "");
        html = std::regex_replace(html, std::regex("<[^>]+>"), " ");
        html = std::regex_replace(html, std::regex("[ \\t]+"), " ");
        html = std::regex_replace(html, std::regex("\\n{2,}"), "\n\n");
        return html;
    }
};
