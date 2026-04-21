#include "chat_gateway.hpp"
#include "config.hpp"
#include "json_util.hpp"
#include <simdjson.h>
#include <spdlog/spdlog.h>

void ChatGateway::init(Agent* agent) {
    agent_ = agent;
    
    auto bridges = Config::instance().bridges();
    for (auto it = bridges.begin(); it != bridges.end(); ++it) {
        std::string provider = it->first.as<std::string>();
        std::string url = it->second["url"].as<std::string>();
        std::string api_key = it->second["api_key"] ? it->second["api_key"].as<std::string>() : "";
        
        spdlog::info("Configuring bridge: {} -> {}", provider, url);
        
        auto client = std::make_unique<WebSocketClient>(url, api_key, [this, provider](const std::string& payload) {
            this->on_message(provider, payload);
        });
        
        clients_[provider] = std::move(client);
    }
}

void ChatGateway::start() {
    for (auto& [name, client] : clients_) {
        client->connect();
    }
}

void ChatGateway::stop() {
    for (auto& [name, client] : clients_) {
        client->disconnect();
    }
}

void ChatGateway::on_message(const std::string& provider, const std::string& json_payload) {
    try {
        simdjson::dom::parser parser;
        simdjson::dom::element doc;
        auto padded = simdjson::padded_string(json_payload);
        if (parser.parse(padded).get(doc)) return;

        std::string_view chat_id_sv, content_sv;
        if (doc["chat_id"].get(chat_id_sv)) return;
        if (doc["content"].get(content_sv)) return;

        std::string chat_id = std::string(chat_id_sv);
        std::string content = std::string(content_sv);
        
        // Session ID mapping: provider:chat_id
        std::string session_id = provider + ":" + chat_id;
        
        spdlog::info("ChatGateway: incoming from {} | session: {} | content: {}", provider, session_id, content);

        // Run the agent. Responses are streamed/sent via callback.
        agent_->run(content, session_id, [this, provider, chat_id](const AgentEvent& ev) {
            if (ev.type == "token") {
                this->send_response(provider, chat_id, ev.content, "token");
            } else if (ev.type == "done") {
                this->send_response(provider, chat_id, ev.content, "done");
            } else if (ev.type == "error") {
                this->send_response(provider, chat_id, ev.content, "error");
            } else if (ev.type == "tool_start") {
                this->send_response(provider, chat_id, ev.content, "tool_start");
            }
        }, provider);

    } catch (const std::exception& e) {
        spdlog::error("Error in ChatGateway::on_message: {}", e.what());
    }
}

void ChatGateway::send_response(const std::string& provider, const std::string& chat_id, const std::string& content, const std::string& type) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    if (clients_.count(provider)) {
        std::string json = "{\"provider\":\"" + json_util::escape(provider) + "\","
                           "\"chat_id\":\"" + json_util::escape(chat_id) + "\","
                           "\"content\":\"" + json_util::escape(content) + "\","
                           "\"type\":\"" + json_util::escape(type) + "\","
                           "\"timestamp\":" + std::to_string(std::time(nullptr)) + "}";
        clients_[provider]->send(json);
    } else {
        spdlog::error("No client found for provider: {}", provider);
    }
}
