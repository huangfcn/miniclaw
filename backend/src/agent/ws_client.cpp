#include "ws_client.hpp"
#include "curl_manager.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

#ifdef _WIN32
#include <fiber.hpp>
#else
#include <fiber.hpp>
#endif

WebSocketClient::WebSocketClient(const std::string& url, const std::string& api_key, MessageCallback callback)
    : url_(url), api_key_(api_key), callback_(callback) {
}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

void WebSocketClient::connect() {
    if (running_.exchange(true)) return;

    easy_ = curl_easy_init();
    curl_easy_setopt(easy_, CURLOPT_URL, url_.c_str());
    
    // Set headers if needed
    struct curl_slist* headers = nullptr;
    if (!api_key_.empty()) {
        std::string auth = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(easy_, CURLOPT_HTTPHEADER, headers);

    // Initial WebSocket options
    curl_easy_setopt(easy_, CURLOPT_CONNECT_ONLY, 2L); // 2L denotes WS 

    // Add to multi manager
    CurlMultiManager::instance().add_handle(easy_);

    // Spawn the loop fiber
    loop_fiber_ = fiber_create([](void* arg) -> void* {
        auto* self = static_cast<WebSocketClient*>(arg);
        self->run_loop();
        return nullptr;
    }, this, nullptr, 64 * 1024);
    fiber_resume(loop_fiber_);
}

void WebSocketClient::disconnect() {
    if (running_.exchange(false)) {
        if (easy_) {
            CurlMultiManager::instance().remove_handle(easy_);
            curl_easy_cleanup(easy_);
            easy_ = nullptr;
        }
        connected_ = false;
    }
}

void WebSocketClient::send(const std::string& message) {
    if (!connected_ || !easy_) return;

    size_t sent;
    CURLcode result = curl_ws_send(easy_, message.c_str(), message.size(), &sent, 0, CURLWS_TEXT);
    if (result != CURLE_OK) {
        spdlog::error("WebSocket send error: {}", curl_easy_strerror(result));
    }
}

void WebSocketClient::run_loop() {
    spdlog::info("WebSocket client loop started for {}", url_);
    
    while (running_.load()) {
        if (!connected_.load()) {
            // Check if connected (handshake done)
            // In libcurl WS, we might need to check the state
            // For now, assume connected after a short time or once we get data
            // Actually, we can check CURLINFO_RESPONSE_CODE
            long response_code = 0;
            curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &response_code);
            if (response_code == 101) {
                if (!connected_.exchange(true)) {
                    spdlog::info("WebSocket connected to {}", url_);
                }
            }
        }

        if (connected_.load()) {
            handle_frames();
        }

#ifdef _WIN32
        boost::this_fiber::sleep_for(std::chrono::milliseconds(50));
#else
        fiber_usleep(50000); // 50ms
#endif
    }
    spdlog::info("WebSocket client loop exiting for {}", url_);
}

void WebSocketClient::handle_frames() {
    const struct curl_ws_frame* meta;
    char buffer[4096];
    size_t nread;
    
    while (true) {
        CURLcode result = curl_ws_recv(easy_, buffer, sizeof(buffer), &nread, &meta);
        
        if (result == CURLE_AGAIN) {
            break; // No more data for now
        } else if (result != CURLE_OK) {
            spdlog::error("WebSocket receive error: {}", curl_easy_strerror(result));
            connected_ = false;
            break;
        }

        receive_buffer_.append(buffer, nread);

        if (meta->bytesleft == 0) {
            // Frame complete
            if (meta->flags & CURLWS_TEXT) {
                if (callback_) {
                    callback_(receive_buffer_);
                }
            }
            receive_buffer_.clear();
        }
    }
}
