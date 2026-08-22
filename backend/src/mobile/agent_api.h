#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// miniclaw mobile C ABI
//
// Thin, stable C interface over the C++ Agent core so that Tauri (or any
// other host) can embed the engine in-process on Android/iOS where spawning
// a sidecar process is not possible.
//
// Threading model:
//   - mc_engine_create() must be called once, from any thread. It starts the
//     internal FiberPool / libuv loop threads.
//   - mc_send_message() may be called from any thread; it runs the agent on
//     an internal fiber and invokes `cb` (possibly multiple times) from a
//     worker thread. The callback must be fast and non-blocking — marshal
//     events to your UI thread yourself.
//   - All string arguments are UTF-8, NUL-terminated. Strings returned by
//     mc_*_get_string() functions are owned by the engine; free them with
//     mc_free_string().
// ─────────────────────────────────────────────────────────────────────────────

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_engine mc_engine;

// Event callback. `type` is one of:
//   "status"      - short progress status line
//   "token"       - streamed assistant text chunk
//   "tool_start"  - tool invocation began (content = "name: args")
//   "tool_end"    - tool result (content = output)
//   "error"       - error message
//   "done"        - turn finished
typedef void (*mc_event_cb)(void *user_data, const char *type, const char *content);

// Create and start the engine.
//   workspace_dir : app-private data directory (e.g. Android filesDir).
//                   All sessions/memory/config live here. Created if missing.
//   config_path   : optional path to config.yaml inside workspace. If NULL,
//                   defaults to <workspace>/config.yaml. If that file does not
//                   exist, engine runs with compiled-in defaults and the UI
//                   can still update settings via mc_set_config_string().
// Returns NULL on failure (see mc_last_error()).
mc_engine *mc_engine_create(const char *workspace_dir, const char *config_path);

// Shut down the engine: stop accepting messages, join worker threads.
void mc_engine_destroy(mc_engine *engine);

// Send a user message for a session. Streaming events are delivered via cb.
// The callback is invoked with type "done" exactly once at the end of the
// turn (or "error" on failure). Safe to call while a previous turn is still
// running; turns are serialized globally (one at a time) and queued.
void mc_send_message(mc_engine *engine, const char *session_id,
                     const char *message, mc_event_cb cb, void *user_data);

// Get the current config file contents (YAML). Returns a malloc'd copy —
// free with mc_free_string(). Returns NULL if no config was loaded.
char *mc_get_config(mc_engine *engine);

// Write new config YAML and apply it. Returns 0 on success, -1 on error.
int mc_set_config(mc_engine *engine, const char *yaml_text);

// Convenience: set a single top-level string setting (e.g. "conversation",
// "api_key"). Values are written into the in-memory config and persisted to
// the config file. Returns 0 on success.
int mc_set_string(mc_engine *engine, const char *section, const char *key,
                  const char *value);

// Engine status: 1 = running, 0 = stopped.
int mc_is_running(mc_engine *engine);

// Human-readable description of the last error (process-wide static buffer).
const char *mc_last_error(void);

// Free any string allocated by this library.
void mc_free_string(char *s);

// Library version string, e.g. "0.1.0".
const char *mc_version(void);

#ifdef __cplusplus
} // extern "C"
#endif
