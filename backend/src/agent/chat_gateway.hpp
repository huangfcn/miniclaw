#pragma once
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include "ws_client.hpp"
#include "agent.hpp"

class ChatGateway {
public:
    static ChatGateway& instance() {
        static ChatGateway inst;
        return inst;
    }

    void init(Agent* agent);
    void start();
    void stop();

    // Standardized message handling
    void on_message(const std::string& provider, const std::string& json_payload);
    
    // Outbound sending
    void send_response(const std::string& provider, const std::string& chat_id, const std::string& content, const std::string& type = "text");

private:
    ChatGateway() = default;

    Agent* agent_ = nullptr;
    std::map<std::string, std::unique_ptr<WebSocketClient>> clients_;
    std::mutex clients_mtx_;
};
