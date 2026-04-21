#include "App.h"
#include <spdlog/spdlog.h>
#include <simdjson.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include "json_util.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

// The Server Bridge acts as the "Server" for both the Frontend (REST) 
// and the Miniclaw Core (WebSocket Client).

struct CoreConnection {
    uWS::WebSocket<false, true, void*>* ws;
    std::string provider;
};

static std::map<uWS::WebSocket<false, true, void*>*, std::string> g_cores;
static std::mutex g_cores_mtx;

// Track pending REST requests to correlate responses
struct PendingRequest {
    uWS::HttpResponse<false>* res;
    bool aborted;
};
static std::map<std::string, std::shared_ptr<PendingRequest>> g_pending_requests;
static std::mutex g_pending_requests_mtx;

static std::atomic<long long> last_ctrl_c_timestamp{0};
static std::atomic<int> ctrl_c_count{0};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT) {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto last = last_ctrl_c_timestamp.exchange(ms);

        if (ms - last <= 1000 && last != 0) {
            const char msg[] = "\nReceived second Ctrl-C, exiting bridge...\n";
            [[maybe_unused]] auto _ = write(STDERR_FILENO, msg, sizeof(msg) - 1);
            _exit(0);
        } else {
            const char msg[] = "\nPress Ctrl-C again within 1s to exit bridge.\n";
            [[maybe_unused]] auto _ = write(STDERR_FILENO, msg, sizeof(msg) - 1);
            return TRUE;
        }
    }
    return FALSE;
}
#else
void signal_handler(int signum) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto last = last_ctrl_c_timestamp.exchange(ms);

    int count = ++ctrl_c_count;
    if (ms - last > 1000) {
        count = 1;
        ctrl_c_count = 1;
    }

    if (count >= 2) {
        const char msg[] = "\nReceived second Ctrl-C, exiting bridge...\n";
        [[maybe_unused]] auto _ = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(0);
    } else {
        const char msg[] = "\nPress Ctrl-C again within 1s to exit bridge.\n";
        [[maybe_unused]] auto _ = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
}
#endif

int main() {
    // Register signal handler for Ctrl-C
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
#endif

    spdlog::info("Starting miniclaw Server Bridge on port 9000");

    uWS::App().get("/api/health", [](auto *res, auto *req) {
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Content-Type", "application/json")
           ->end("{\"status\":\"OK\",\"mode\":\"bridge\"}");
    }).options("/*", [](auto *res, auto *req) {
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
           ->writeHeader("Access-Control-Allow-Headers", "content-type, authorization")
           ->end();
    }).ws<void*>("/api/chat/stream", {
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 16 * 1024 * 1024,
        .idleTimeout = 120,
        .open = [](auto *ws) {
            spdlog::info("Miniclaw Core connected via WebSocket");
            std::lock_guard<std::mutex> lock(g_cores_mtx);
            g_cores[ws] = "unknown"; // Will be updated on first message
        },
        .message = [](auto *ws, std::string_view message, uWS::OpCode opCode) {
            // Handle standardized response from Core
            try {
                simdjson::dom::parser parser;
                simdjson::dom::element doc;
                if (parser.parse(message.data(), message.length()).get(doc)) return;

                std::string_view chat_id_sv, content_sv, type_sv;
                if (doc["chat_id"].get(chat_id_sv)) return;
                if (doc["content"].get(content_sv)) return;

                std::string type = "token";
                if (!doc["type"].get(type_sv)) {
                    type = std::string(type_sv);
                }

                std::string chat_id = std::string(chat_id_sv);
                std::string content = std::string(content_sv);

                // Relay back to the frontend REST request if it exists
                std::lock_guard<std::mutex> lock(g_pending_requests_mtx);
                if (g_pending_requests.count(chat_id)) {
                    auto req = g_pending_requests[chat_id];
                    if (!req->aborted) {
                        // Translate back to the SSE format the frontend expects
                        std::string sse_data;
                        if (type == "error") {
                            sse_data = "data: {\"type\":\"error\",\"content\":\"" + json_util::escape(content) + "\"}\n\n";
                        } else {
                            sse_data = "data: {\"type\":\"" + json_util::escape(type) + "\",\"content\":\"" + json_util::escape(content) + "\"}\n\n";
                        }
                        
                        req->res->write(sse_data);
                        
                        if (type == "done" || type == "error") {
                            req->res->end();
                            g_pending_requests.erase(chat_id);
                        }
                    } else {
                        g_pending_requests.erase(chat_id);
                    }
                }
            } catch (...) {}
        },
        .close = [](auto *ws, int code, std::string_view message) {
            spdlog::info("Miniclaw Core disconnected");
            std::lock_guard<std::mutex> lock(g_cores_mtx);
            g_cores.erase(ws);
        }
    }).post("/api/chat", [](auto *res, auto *req) {
        // Legacy REST API for the frontend
        res->onAborted([res]() {
            // Mark as aborted
        });

        res->onData([res](std::string_view data, bool last) {
            static thread_local std::string body;
            body.append(data.data(), data.length());
            if (last) {
                try {
                    simdjson::dom::parser parser;
                    simdjson::dom::element x;
                    if (parser.parse(body).get(x)) {
                        res->writeStatus("400 Bad Request")->end("Invalid JSON");
                        body.clear();
                        return;
                    }

                    std::string_view message_sv, session_id_sv;
                    if (x["message"].get(message_sv)) {}
                    if (x["session_id"].get(session_id_sv)) {}

                    std::string chat_id = "bridge-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                    
                    auto pending = std::make_shared<PendingRequest>();
                    pending->res = res;
                    pending->aborted = false;
                    
                    {
                        std::lock_guard<std::mutex> lock(g_pending_requests_mtx);
                        g_pending_requests[chat_id] = pending;
                    }

                    res->onAborted([chat_id]() {
                        std::lock_guard<std::mutex> lock(g_pending_requests_mtx);
                        if (g_pending_requests.count(chat_id)) {
                            g_pending_requests[chat_id]->aborted = true;
                        }
                    });

                    res->writeStatus("200 OK")
                       ->writeHeader("Content-Type", "text/event-stream")
                       ->writeHeader("Cache-Control", "no-cache")
                       ->writeHeader("Connection", "keep-alive")
                       ->writeHeader("Access-Control-Allow-Origin", "*");

                    // Forward to connected Miniclaw Core
                    std::lock_guard<std::mutex> core_lock(g_cores_mtx);
                    if (!g_cores.empty()) {
                        auto* core_ws = g_cores.begin()->first;
                        std::string core_json = "{\"chat_id\":\"" + json_util::escape(chat_id) + "\","
                                                "\"content\":\"" + json_util::escape(std::string(message_sv)) + "\","
                                                "\"provider\":\"legacy_frontend\"}";
                        core_ws->send(core_json, uWS::OpCode::TEXT);
                    } else {
                        res->write("data: {\"type\":\"error\",\"content\":\"No Miniclaw Core connected\"}\n\n");
                        res->end();
                    }
                } catch (...) {
                    res->writeStatus("500 Internal Server Error")->end();
                }
                body.clear();
            }
        });
    }).post("/v1/chat/completions", [](auto *res, auto *req) {
        // OpenAI-compatible REST API
        res->onData([res](std::string_view data, bool last) {
            static thread_local std::string body;
            body.append(data.data(), data.length());
            if (last) {
                try {
                    simdjson::dom::parser parser;
                    simdjson::dom::element x;
                    if (parser.parse(body).get(x)) {
                        res->writeStatus("400 Bad Request")->end("Invalid JSON");
                        body.clear();
                        return;
                    }

                    std::string message = "";
                    simdjson::dom::array messages;
                    if (!x["messages"].get(messages) && messages.size() > 0) {
                        simdjson::dom::element last_msg;
                        if (!messages.at(messages.size() - 1).get(last_msg)) {
                            std::string_view content_sv;
                            if (!last_msg["content"].get(content_sv)) message = std::string(content_sv);
                        }
                    }

                    std::string chat_id = "openai-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                    
                    auto pending = std::make_shared<PendingRequest>();
                    pending->res = res;
                    pending->aborted = false;
                    
                    {
                        std::lock_guard<std::mutex> lock(g_pending_requests_mtx);
                        g_pending_requests[chat_id] = pending;
                    }

                    res->onAborted([chat_id]() {
                        std::lock_guard<std::mutex> lock(g_pending_requests_mtx);
                        if (g_pending_requests.count(chat_id)) g_pending_requests[chat_id]->aborted = true;
                    });

                    res->writeStatus("200 OK")
                       ->writeHeader("Content-Type", "text/event-stream")
                       ->writeHeader("Cache-Control", "no-cache")
                       ->writeHeader("Connection", "keep-alive")
                       ->writeHeader("Access-Control-Allow-Origin", "*");

                    // Forward to connected Miniclaw Core
                    std::lock_guard<std::mutex> core_lock(g_cores_mtx);
                    if (!g_cores.empty()) {
                        auto* core_ws = g_cores.begin()->first;
                        std::string core_json = "{\"chat_id\":\"" + json_util::escape(chat_id) + "\","
                                                "\"content\":\"" + json_util::escape(message) + "\","
                                                "\"provider\":\"openai_bridge\"}";
                        core_ws->send(core_json, uWS::OpCode::TEXT);
                    } else {
                        res->write("data: {\"error\": \"No Miniclaw Core connected\"}\n\n");
                        res->end();
                    }
                } catch (...) {
                    res->writeStatus("500 Internal Server Error")->end();
                }
                body.clear();
            }
        });
    }).listen(9000, [](auto *listen_socket) {
        if (listen_socket) {
            spdlog::info("Server Bridge listening on port 9000");
        }
    }).run();

    return 0;
}
