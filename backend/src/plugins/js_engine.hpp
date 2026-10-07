#pragma once
// QuickJS-based JS engine for tool plugins (quickjs-ng).
//
// quickjs-ng ownership rules (see quickjs.h header comment):
//   - a function taking a JSValue parameter TAKES ownership of it;
//   - a function taking a JSValueConst parameter does NOT take ownership;
//   - a function returning a JSValue transfers ownership to the caller.
// In particular, JS_SetPropertyStr(ctx, obj, name, val) CONSUMES val on
// every path (success: the property holds it; failure: it is freed inside).
// Never free a value after passing it to JS_SetPropertyStr.
//
// Thread model: process-wide singleton owning one JSRuntime + mutex. Each
// call() creates a fresh JSContext, evaluates the plugin source with an
// `api` object bound, invokes the handler, and frees the context.
//
// The only host capabilities visible to JS are the `api` binding (see
// js_plugin.hpp). QuickJS has no filesystem/network by default, so anything
// not attached here is unreachable.

#include <quickjs.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace plugins {

// Result of a host HTTP fetch (see js_plugin.hpp::js_http_fetch).
struct JsFetchResult {
     long status = 0;     // 0 = transport error
     std::string body;
};

// Host capabilities exposed to plugins as the global `api` object.
// All std::function members are optional; an unset member makes the
// corresponding api function return an error in JS.
struct JsApi {
     // api.fetch(url, {method, headers, body}) -> {status, body}
     std::function<JsFetchResult(const std::string &url,
                                 const std::string &method,
                                 const std::vector<std::string> &headers,
                                 const std::string &body)>
         fetch;
     // api.readFile(path) -> string (null on failure); sandboxed to workspace
     std::function<std::string(const std::string &path)> read_file;
     // api.writeFile(path, content) -> boolean; sandboxed to workspace
     std::function<bool(const std::string &path,
                         const std::string &content)>
         write_file;
     // api.config(section, key) -> string
     std::function<std::string(const std::string &section,
                                const std::string &key)>
         config;
     // api.log(msg) — forwarded to spdlog
     std::function<void(const std::string &msg)> log;
     // api.now() — ISO 8601 local timestamp
     std::string (*now)(void);
     // api.workspace() -> absolute workspace path
     std::string workspace;
};

struct JsCallResult {
     bool ok = false;
     std::string value;  // handler's string return
     std::string error;  // populated on failure
};

// NOTE: must be a process-wide singleton (NOT thread_local). Tool handlers
// run on pool fibers, and js_http_fetch suspends the fiber mid-call; the
// fiber resumes on the curl manager thread. If each OS thread had its own
// JSRuntime, the JS_Call continuation (JS_FreeValue/JS_FreeContext) would
// touch the *originating* thread's runtime from a different thread,
// corrupting QuickJS's GC lists. A single runtime + mutex serializes all
// JS access, which is the only safe model for a non-thread-safe runtime.
class JsEngine {
public:
     static JsEngine &instance() {
         static JsEngine eng;
         return eng;
     }
     JsEngine(const JsEngine &) = delete;
     JsEngine &operator=(const JsEngine &) = delete;

     // Operation budget before the interrupt handler aborts execution
     // (guards against infinite loops in plugin code).
     static constexpr int64_t kOpLimit = 50'000'000;

     // Evaluate `source` in a fresh context (with `api` bound), then call the
     // global function `handler` once with a JS object built from `args`.
     // Returns the handler's string result or an error message.
     //
     // The whole call (including any fiber suspension inside api.fetch) runs
     // under the engine lock, so no other thread can touch the runtime while
     // this JS context is live — even if the current fiber migrates threads.
     JsCallResult call(const std::string &source,
                        const std::string &handler,
                        const std::map<std::string, std::string> &args,
                        const JsApi &api) {
         std::lock_guard<std::mutex> lk(mu_);
         return call_unlocked(source, handler, args, api);
     }

private:
     JsCallResult call_unlocked(const std::string &source,
                                 const std::string &handler,
                                 const std::map<std::string, std::string> &args,
                                 const JsApi &api) {
         JsCallResult r;
         if (!ensure_runtime()) {
             r.error = "failed to initialize JS runtime";
             return r;
         }
         JSContext *ctx = JS_NewContext(rt_);
         if (!ctx) {
             r.error = "failed to create JS context";
             return r;
         }
         ops_ = 0;
         api_ = &api;

         if (!bind_api(ctx)) {
             r.error = "failed to bind api";
             JS_FreeContext(ctx);
             return r;
         }

         JSValue ev = JS_Eval(ctx, source.c_str(), source.size(), "<plugin.js>",
                              0);
         if (JS_IsException(ev)) {
             r.error = "plugin eval failed: " + js_exception(ctx);
         } else {
             JS_FreeValue(ctx, ev);
             JSValue global = JS_GetGlobalObject(ctx);
             JSValue fn = JS_GetPropertyStr(ctx, global, handler.c_str());
             JS_FreeValue(ctx, global);
             if (JS_IsException(fn)) {
                 r.error = "plugin handler lookup failed: " + js_exception(ctx);
             } else if (!JS_IsFunction(ctx, fn)) {
                 r.error = "handler '" + handler + "' not found in plugin";
             } else {
                 JSValue argv0 = JS_NewObject(ctx);
                 for (const auto &[k, v] : args)
                     JS_SetPropertyStr(ctx, argv0, k.c_str(),
                                        JS_NewStringLen(ctx, v.data(), v.size()));
                 JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &argv0);
                 JS_FreeValue(ctx, argv0);
                 JS_FreeValue(ctx, fn);
                 if (JS_IsException(ret)) {
                     r.error = js_exception(ctx);
                 } else if (JS_IsObject(ret) && !JS_IsString(ret)) {
                     r.error = "plugin handler must return a string (got an "
                               "object; return JSON.stringify(...) instead)";
                 } else {
                     const char *s = JS_ToCString(ctx, ret);
                     if (s) {
                         r.value = s;
                         JS_FreeCString(ctx, s);
                         r.ok = true;
                     } else {
                         r.error = js_exception(ctx);
                     }
                 }
                 JS_FreeValue(ctx, ret);
             }
         }
         JS_FreeContext(ctx);
         api_ = nullptr;
         if (!r.ok && r.error.empty()) r.error = "unknown JS error";
         return r;
     }

     ~JsEngine() {
         if (rt_) {
             JS_FreeRuntime(rt_);
             rt_ = nullptr;
         }
     }

     // Serializes all JS runtime access (see class comment).
     std::mutex mu_;

private:
     JsEngine() = default;

     bool ensure_runtime() {
         if (rt_) return true;
         rt_ = JS_NewRuntime();
         if (!rt_) return false;
         JS_SetInterruptHandler(rt_, &interrupt_cb, this);
         return true;
     }

     static int interrupt_cb(JSRuntime *, void *opaque) {
         auto *e = static_cast<JsEngine *>(opaque);
         return ++e->ops_ > kOpLimit;
     }

     static std::string js_exception(JSContext *ctx) {
         JSValue e = JS_GetException(ctx);
         std::string msg;
         if (!JS_IsUndefined(e)) {
             const char *s = JS_ToCString(ctx, e);
             if (s) {
                 msg = s;
                 JS_FreeCString(ctx, s);
             }
         }
         JS_FreeValue(ctx, e);
         return msg.empty() ? "unknown error" : msg;
     }

     // Read a JS string argument; returns empty string on non-strings.
     static std::string js_str(JSContext *ctx, JSValueConst v) {
         if (JS_IsString(v)) {
             const char *s = JS_ToCString(ctx, v);
             std::string out;
             if (s) {
                 out = s;
                 JS_FreeCString(ctx, s);
             }
             return out;
         }
         return "";
     }

     static std::vector<std::string> js_header_list(JSContext *ctx,
                                                     JSValueConst v) {
         std::vector<std::string> out;
         if (JS_IsArray(v)) {
             int64_t len = 0;

             if (JS_GetLength(ctx, v, &len)) return out;

             for (int64_t i = 0; i < len; ++i) {

                  JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
                 if (!JS_IsException(el)) {
                     out.push_back(js_str(ctx, el));
                     JS_FreeValue(ctx, el);
                 }
             }
         } else if (JS_IsObject(v)) {
             JSPropertyEnum *tab = nullptr;

             uint32_t plen = 0;

             if (!JS_GetOwnPropertyNames(ctx, &tab, &plen, v, 0)) {

                  for (uint32_t i = 0; i < plen; ++i) {

                      const char *k = JS_AtomToCString(ctx, tab[i].atom);


                      if (!k) continue;


                      std::string key(k);


                      JS_FreeCString(ctx, k);


                      JSValue val = JS_GetPropertyStr(ctx, v, key.c_str());

                      out.push_back(key + ": " + js_str(ctx, val));

                      JS_FreeValue(ctx, val);

                  }

                  JS_FreePropertyEnum(ctx, tab, plen);

             }
         }
         return out;
     }

     static JSValue mk_error(JSContext *ctx, const char *msg) {
         std::string m = msg ? msg : "";
         JSValue e = JS_NewError(ctx);
         JS_SetPropertyStr(ctx, e, "message",
                            JS_NewStringLen(ctx, m.data(), m.size()));
         return e;
     }

     // ── api.* bindings ──────────────────────────────────────────────────
     static JSValue api_fetch(JSContext *ctx, JSValueConst, int argc,
                              JSValueConst *argv) {
         auto &eng = instance();
         if (!eng.api_ || !eng.api_->fetch)
             return JS_ThrowTypeError(ctx, "api.fetch not available");
         std::string url = js_str(ctx, argv[0]);
         std::string method = "GET";
         std::vector<std::string> headers;
         std::string body;
         if (argc > 1 && JS_IsObject(argv[1])) {
             JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
             std::string ms = js_str(ctx, m);
             JS_FreeValue(ctx, m);
             if (!ms.empty()) method = ms;
             JSValue h = JS_GetPropertyStr(ctx, argv[1], "headers");
             headers = js_header_list(ctx, h);
             JS_FreeValue(ctx, h);
             JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
             body = js_str(ctx, b);
             JS_FreeValue(ctx, b);
         }
         auto fr = eng.api_->fetch(url, method, headers, body);
         JSValue o = JS_NewObject(ctx);
         JS_SetPropertyStr(ctx, o, "status", JS_NewInt64(ctx, fr.status));
         JS_SetPropertyStr(ctx, o, "body",
                            JS_NewStringLen(ctx, fr.body.data(), fr.body.size()));
         return o;
     }

     static JSValue api_read_file(JSContext *ctx, JSValueConst, int argc,
                                  JSValueConst *argv) {
         auto &eng = instance();
         if (!eng.api_ || !eng.api_->read_file)
             return JS_ThrowTypeError(ctx, "api.readFile not available");
         if (argc < 1) return JS_ThrowTypeError(ctx, "api.readFile(path)");
         std::string content = eng.api_->read_file(js_str(ctx, argv[0]));
         if (content.empty()) return JS_UNDEFINED;
         return JS_NewStringLen(ctx, content.data(), content.size());
     }

     static JSValue api_write_file(JSContext *ctx, JSValueConst, int argc,
                                   JSValueConst *argv) {
         auto &eng = instance();
         if (!eng.api_ || !eng.api_->write_file)
             return JS_ThrowTypeError(ctx, "api.writeFile not available");
         if (argc < 2)
             return JS_ThrowTypeError(ctx, "api.writeFile(path, content)");
         bool ok =
             eng.api_->write_file(js_str(ctx, argv[0]), js_str(ctx, argv[1]));
         return JS_NewBool(ctx, ok);
     }

     static JSValue api_config(JSContext *ctx, JSValueConst, int argc,
                               JSValueConst *argv) {
         auto &eng = instance();
         if (!eng.api_ || !eng.api_->config)
             return JS_ThrowTypeError(ctx, "api.config not available");
         if (argc < 2) return JS_ThrowTypeError(ctx, "api.config(section, key)");
         std::string v = eng.api_->config(js_str(ctx, argv[0]),
                                           js_str(ctx, argv[1]));
         return JS_NewStringLen(ctx, v.data(), v.size());
     }

     static JSValue api_log(JSContext *ctx, JSValueConst, int argc,
                            JSValueConst *argv) {
         auto &eng = instance();
         if (eng.api_ && eng.api_->log && argc > 0)
             eng.api_->log(js_str(ctx, argv[0]));
         return JS_UNDEFINED;
     }

     static JSValue api_now(JSContext *ctx, JSValueConst, int,
                            JSValueConst *) {
         auto &eng = instance();
         std::string t = (eng.api_ && eng.api_->now) ? eng.api_->now() : "";
         return JS_NewStringLen(ctx, t.data(), t.size());
     }

     static JSValue api_workspace(JSContext *ctx, JSValueConst, int,
                                  JSValueConst *) {
         auto &eng = instance();
         const std::string &w = eng.api_ ? eng.api_->workspace : "";
         return JS_NewStringLen(ctx, w.data(), w.size());
     }

     bool bind_api(JSContext *ctx) {
         JSValue o = JS_NewObject(ctx);
         if (JS_IsException(o)) return false;
         JSValue fns[7] = {
             JS_NewCFunction(ctx, &api_fetch, "fetch", 2),
             JS_NewCFunction(ctx, &api_read_file, "readFile", 1),
             JS_NewCFunction(ctx, &api_write_file, "writeFile", 2),
             JS_NewCFunction(ctx, &api_config, "config", 2),
             JS_NewCFunction(ctx, &api_log, "log", 1),
             JS_NewCFunction(ctx, &api_now, "now", 0),
             JS_NewCFunction(ctx, &api_workspace, "workspace", 0),
         };
         for (auto &f : fns) {
             if (JS_IsException(f)) {
                 for (auto &g : fns) JS_FreeValue(ctx, g);
                 JS_FreeValue(ctx, o);
                 return false;
             }
         }
         const char *names[7] = {"fetch", "readFile", "writeFile", "config",
                                  "log", "now", "workspace"};
         // JS_SetPropertyStr CONSUMES each value (quickjs-ng ownership
         // rules): the property holds fns[i] on success and the call frees
         // it on failure. Freeing them again here was a double-free that
         // corrupted the GC list.
         for (int i = 0; i < 7; ++i)
             JS_SetPropertyStr(ctx, o, names[i], fns[i]);
         JSValue global = JS_GetGlobalObject(ctx);
         // Consumes `o` — do not free it afterwards.
         JS_SetPropertyStr(ctx, global, "api", o);
         JS_FreeValue(ctx, global);
         return true;
     }

     JSRuntime *rt_ = nullptr;
     int64_t ops_ = 0;
     const JsApi *api_ = nullptr;
};

} // namespace plugins
