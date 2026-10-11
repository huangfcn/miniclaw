#pragma once
// WebSearchTool + WebFetchTool — with native function-calling schema

#include "tool.hpp"
#include "../config.hpp"
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstring>
#include <cctype>
#include <simdjson.h>
#include <curl/curl.h>
#include <fiber.hpp>
#include "../agent/curl_manager.hpp"


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
        // SAFETY: Called by CurlMultiManager on the thread that initiated the fetch (owning fiber thread).
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
        // Resolved lazily (not in the constructor) so keys pushed at runtime
        // via mc_set_string("web", "brave_api_key", ...) from the mobile UI
        // take effect without an engine restart. Env var still works on desktop.
        const std::string api_key = Config::instance().web_brave_api_key();
        if (api_key.empty()) return "Error: BRAVE_API_KEY not configured";

        char* encoded = curl_easy_escape(nullptr, input.c_str(), input.length());
        std::string url = "https://api.search.brave.com/res/v1/web/search?q=" + std::string(encoded);
        curl_free(encoded);

        std::string res = curl_fetch(url, {"X-Subscription-Token: " + api_key});
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
    // Case-insensitive find of `needle` in s starting at pos.
    static size_t find_ci(const std::string& s, const char* needle, size_t pos) {
        size_t nl = strlen(needle);
        for (size_t i = pos; i + nl <= s.size(); ++i) {
            bool ok = true;
            for (size_t j = 0; j < nl; ++j)
                if (tolower((unsigned char)s[i + j]) != tolower((unsigned char)needle[j])) { ok = false; break; }
            if (ok) return i;
        }
        return std::string::npos;
    }

    // Linear-time HTML -> text. The old std::regex version (<script[\s\S]*?</script>)
    // recursed per character in libstdc++'s backtracking engine and overflowed the
    // thread stack on multi-MB pages (SIGSEGV in _Executor::_M_dfs).
    static std::string strip_html(std::string html) {
        std::string out;
        out.reserve(html.size() / 4 + 64);
        size_t i = 0, n = html.size();
        while (i < n) {
            char c = html[i];
            if (c == '<') {
                // Find the end of this tag.
                size_t end = html.find('>', i);
                if (end == std::string::npos) { out += ' '; break; } // stray '<'
                std::string tag = html.substr(i, end - i);
                // Case-insensitive prefix check for script/style blocks.
                auto starts_with_ci = [&](const char* w) {
                    size_t wl = strlen(w);
                    if (tag.size() < wl + 1) return false; // need "<" + word
                    for (size_t k = 1; k <= wl; ++k)
                        if (tolower((unsigned char)tag[k]) != tolower((unsigned char)w[k - 1])) return false;
                    // Word occupies tag[1..wl]; boundary is tag[wl+1] or the closing '>'.
                    char nxt = (tag.size() > wl + 1) ? tag[wl + 1] : '>';
                    return nxt == ' ' || nxt == '\t' || nxt == '\n' || nxt == '\r' || nxt == '/' ;
                };
                if (starts_with_ci("script") || starts_with_ci("style")) {
                    // Skip to the matching closing tag.
                    std::string close = std::string("</") + (starts_with_ci("script") ? "script" : "style");
                    size_t cp = find_ci(html, close.c_str(), end);
                    if (cp == std::string::npos) { break; } // unterminated: drop the rest
                    size_t ce = html.find('>', cp);
                    i = (ce == std::string::npos) ? n : ce + 1;
                    continue;
                }
                out += ' ';
                i = end + 1;
                continue;
            }
            out += c;
            ++i;
        }
        // Compress whitespace: runs of spaces/tabs -> one space; 3+ newlines -> two.
        std::string res;
        res.reserve(out.size());
        int blank_run = 0, nl_run = 0;
        for (char c : out) {
            if (c == ' ' || c == '\t') { if (blank_run == 0 && !res.empty()) res += ' '; blank_run++; nl_run = 0; }
            else if (c == '\n') { nl_run++; blank_run = 0; if (nl_run <= 2) res += c; }
            else { res += c; blank_run = 0; nl_run = 0; }
        }
        return res;
    }
};
