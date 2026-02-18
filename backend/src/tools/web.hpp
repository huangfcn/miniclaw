#pragma once
// WebTools — mirrors nanobot/nanobot/agent/tools/web.py
// web_search: Search the web using Brave Search API
// web_fetch:  Fetch a URL and extract text content (HTML -> text)

#include "tool.hpp"
#include <string>
#include <vector>
#include <regex>
#include <nlohmann/json.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

using json = nlohmann::json;

class WebSearchTool : public Tool {
public:
    WebSearchTool() {
        api_key_ = std::getenv("BRAVE_API_KEY") ? std::getenv("BRAVE_API_KEY") : "";
    }

    std::string execute(const std::string& input) override {
        if (api_key_.empty()) return "Error: BRAVE_API_KEY not configured";

        std::string query = input;
        // Simple search logic using Brave API
        httplib::Client cli("https://api.search.brave.com");
        cli.set_header_authorizing("X-Subscription-Token", api_key_);
        
        auto res = cli.Get("/res/v1/web/search?q=" + httplib::detail::encode_url(query));
        if (!res || res->status != 200) {
            return "Error: Search failed (HTTP " + std::to_string(res ? res->status : 0) + ")";
        }

        try {
            auto j = json::parse(res->body);
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
    std::string execute(const std::string& input) override {
        std::string url = input;
        
        // Use httplib to fetch the URL
        // Note: For simplicity, we handle only simple URLs for now
        // A full implementation would need to parse the domain and scheme
        std::regex url_re(R"(^(https?://)([^/]+)(.*)$)", std::regex::icase);
        std::smatch m;
        if (!std::regex_match(url, m, url_re)) {
            return "Error: Invalid URL format. Must start with http:// or https://";
        }

        std::string scheme = m[1];
        std::string host = m[2];
        std::string path = m[3].str().empty() ? "/" : m[3].str();

        httplib::Client cli(scheme + host);
        cli.set_follow_location(true);
        cli.set_read_timeout(10, 0);
        
        auto res = cli.Get(path);
        if (!res || res->status != 200) {
            return "Error: Fetch failed (HTTP " + std::to_string(res ? res->status : 0) + ")";
        }

        std::string html = res->body;
        return "URL: " + url + "\nContent (first 2000 chars):\n\n" + strip_html(html).substr(0, 2000);
    }

private:
    std::string strip_html(std::string html) {
        // Very basic HTML tag removal
        html = std::regex_replace(html, std::regex("<script[\\s\\S]*?</script>", std::regex::icase), "");
        html = std::regex_replace(html, std::regex("<style[\\s\\S]*?</style>", std::regex::icase), "");
        html = std::regex_replace(html, std::regex("<[^>]+>"), " ");
        
        // Normalize whitespace
        html = std::regex_replace(html, std::regex("[ \\t]+"), " ");
        html = std::regex_replace(html, std::regex("\\n{2,}"), "\n\n");
        return html;
    }
};
