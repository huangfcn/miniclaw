#pragma once
#include <uv.h>
#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <string>
#include <functional>

struct CurlContext {
    uv_poll_t poll_handle;
    curl_socket_t sockfd;
    class CurlMultiManager* manager;
};

class CurlMultiManager {
public:
    static CurlMultiManager& instance() {
        static thread_local CurlMultiManager inst;
        return inst;
    }

    void init(uv_loop_t* loop) {
        loop_ = loop;
        multi_ = curl_multi_init();
        curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, socket_callback);
        curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
        curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, timer_callback);
        curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
        
        uv_timer_init(loop, &timer_handle_);
        timer_handle_.data = this;
    }

    CURLM* multi() { return multi_; }

    void add_handle(CURL* easy) {
        curl_multi_add_handle(multi_, easy);
    }

    void remove_handle(CURL* easy) {
        curl_multi_remove_handle(multi_, easy);
    }

    void check_multi_info() {
        int msgs_left;
        CURLMsg* msg;
        while ((msg = curl_multi_info_read(multi_, &msgs_left))) {
            if (msg->msg == CURLMSG_DONE) {
                CURL* easy = msg->easy_handle;
                void* userp;
                curl_easy_getinfo(easy, CURLINFO_PRIVATE, &userp);
                if (userp) {
                    auto* notify_fn = (std::function<void(CURLcode)>*)userp;
                    (*notify_fn)(msg->data.result);
                }
            }
        }
    }

private:
    CurlMultiManager() = default;

    static int socket_callback(CURL* easy, curl_socket_t s, int action, void* userp, void* socketp) {
        auto* self = (CurlMultiManager*)userp;
        auto* ctx = (CurlContext*)socketp;

        if (action == CURL_POLL_IN || action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
            if (!ctx) {
                ctx = new CurlContext();
                ctx->sockfd = s;
                ctx->manager = self;
                uv_poll_init_socket(self->loop_, &ctx->poll_handle, s);
                ctx->poll_handle.data = ctx;
                curl_multi_assign(self->multi_, s, ctx);
            }

            int events = 0;
            if (action != CURL_POLL_OUT) events |= UV_READABLE;
            if (action != CURL_POLL_IN)  events |= UV_WRITABLE;

            uv_poll_start(&ctx->poll_handle, events, [](uv_poll_t* handle, int status, int events) {
                auto* ctx = (CurlContext*)handle->data;
                int flags = 0;
                if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
                if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;

                int running_handles;
                curl_multi_socket_action(ctx->manager->multi(), ctx->sockfd, flags, &running_handles);
                ctx->manager->check_multi_info();
            });
        } else if (action == CURL_POLL_REMOVE) {
            if (ctx) {
                uv_poll_stop(&ctx->poll_handle);
                uv_close((uv_handle_t*)&ctx->poll_handle, [](uv_handle_t* h) {
                    delete (CurlContext*)h->data;
                });
                curl_multi_assign(self->multi_, s, nullptr);
            }
        }
        return 0;
    }

    static int timer_callback(CURLM* multi, long timeout_ms, void* userp) {
        auto* self = (CurlMultiManager*)userp;
        if (timeout_ms < 0) {
            uv_timer_stop(&self->timer_handle_);
        } else {
            if (timeout_ms == 0) timeout_ms = 1;
            uv_timer_start(&self->timer_handle_, [](uv_timer_t* handle) {
                auto* self = (CurlMultiManager*)handle->data;
                int running_handles;
                curl_multi_socket_action(self->multi(), CURL_SOCKET_TIMEOUT, 0, &running_handles);
                self->check_multi_info();
            }, timeout_ms, 0);
        }
        return 0;
    }
    
    CURLM* multi_ = nullptr;
    uv_loop_t* loop_ = nullptr;
    uv_timer_t timer_handle_;
};
