#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Internal hooks for the miniclaw mobile engine. NOT part of the public C ABI
// (agent_api.h) — only consumed by host bridges living in this directory
// (e.g. the JNI bridge).
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>

namespace mc {

// Called on the engine's turn worker thread after a turn has fully completed
// (all events delivered, including any trailing "done"). Hosts that allocate
// per-turn state alongside their event callback should release it here.
// Must be fast and non-blocking. Set once at bridge init; nullptr = no-op.
using TurnCleanupFn = void (*)(void *user_data);

extern std::atomic<TurnCleanupFn> g_turn_cleanup;

} // namespace mc
