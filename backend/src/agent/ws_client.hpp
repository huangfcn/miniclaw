#pragma once
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <curl/curl.h>
#include <fiber.hpp>
#include <atomic>

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string& json_payload)>;

    WebSocketClient(const std::string& url, const std::string& api_key, MessageCallback callback);
    ~WebSocketClient();

    void connect();
    void disconnect();
    void send(const std::string& message);
    
    bool is_connected() const { return connected_.load(); }
    const std::string& url() const { return url_; }

private:
    void run_loop();
    void handle_frames();

    std::string url_;
    std::string api_key_;
    MessageCallback callback_;
    
    CURL* easy_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    fiber_t loop_fiber_;
    
    std::string receive_buffer_;
};
