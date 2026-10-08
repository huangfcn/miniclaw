# miniclaw 🦞🦀

**A personal AI agent that lives on your phone.** No home server, no cloud
service, no computer setup — install the app, and the agent runs in-process
inside the app's own sandbox. The more you use it, the better it knows you:
its memory is distilled files on your device, not an opaque cloud blob.

Desktop and cloud agents (OpenClaw & co.) are powerful, but they ask you to
solve a *deployment problem* first — a machine that stays on, a service that
stays up, a setup most people never do. miniclaw inverts that: the phone is
the one computer you already carry, always have on, and already trust with
your identity. The agent ships pre-deployed, because it's just an app.

---

## What it does

A ReAct agent (Thought → Action → Observation) with pluggable tools, tuned
for **life-shaped jobs** that only need network access:

| Job | How |
|---|---|
| Daily news / finance digest | `web_search` + `web_fetch` (libcurl), cron-scheduled |
| Email triage | Gmail tool (OAuth in the sandbox, IMAP over libcurl) |
| Travel & hotel research | JS plugin (`travel`) — search + fetch, partner APIs where available |
| Persistent memory | 3-stage distillation + hybrid vector/keyword search |

**Extensible without recompiling** — tools can also be written in plain
JavaScript: drop a folder in `workspace/plugins/<name>/` (`plugin.json` +
`plugin.js`) and it's registered at startup, running in a sandboxed
QuickJS-ng v0.17.0 with a small `api.*` surface (fetch, files, config,
log). Skills work the same way — `workspace/skills/<name>/SKILL.md` folders
are parsed and injected into the system prompt. See [docs/PLUGINS.md](docs/PLUGINS.md).

**Model strategy** — three independent, config-driven endpoints, each with
its own provider:

- `conversation` → a strong API model (the agent's reasoning)
- `memory` → the distillation model
- `embedding` → the RAG vectorizer

All three accept `provider: mnn` for **on-device** inference — Qwen3-1.7B
(chat/distillation) and BGE-M3 (embeddings) run in-process via MNN, no
network, no API key ([docs/LOCAL_MODELS.md](docs/LOCAL_MODELS.md)). The
default stays on the APIs you configure; switching a role to on-device is a
config change, not an app change.

## How it learns you

Every conversation feeds a 3-stage distillation pipeline:

```
sessions/*.jsonl (raw) ──► memory/YYYY-MM-DD.md (daily summaries)
                              ──► MEMORY.md / USER.md (permanent facts & preferences)
```

Retrieval is hybrid — Faiss (vector) + keyword index (Lucene++, or SQLite
FTS5 on Apple platforms and via `-DUSE_SQLITE=ON` elsewhere),
reciprocal-rank fused with temporal decay. The result compounds: preferences stated once
("under $120, window seat") become standing context weeks later. And because
the "brain" is plain files in the app's workspace, **you can read, edit, and
export everything the agent believes about you.** That inspectability is a
core design principle — transparency over magic.

## The sandbox is the feature

The agent runs inside the Android/iOS app sandbox with deliberately minimal
privilege:

| | |
|---|---|
| **Permissions** | `INTERNET` only. No storage, no contacts, no location. |
| **Files** | Confined to the app's private workspace (`filesDir/workspace`). Every file-tool call is path-checked against it, including `..` escapes. |
| **Shell** | No fork/exec, ever. The `exec` tool is a C++-implemented POSIX-style shell (`mobile_shell`: ls/cat/grep/find/cp/mv/rm + pipelines) that can only touch the workspace. |
| **Network** | Full outbound (LLM APIs, search, fetch). Cleartext HTTP allowed for local-network servers. |

It's a full-power agent *inside its own home directory* — strong enough for
its jobs, impossible to point at other apps' data or the host system.

## Architecture

One C++ engine, three frontends:

```
┌────────────────────────────────────────────────────────────────┐
│  Frontends                                                     │
│   • Tauri + React (desktop)      — miniclaw sidecar            │
│     (thin host over the same C ABI, HTTP/SSE on :9000)         │
│   • Native Kotlin app (Android)  — in-process, JNI bridge      │
│   • Native SwiftUI app (iOS)     — in-process, Clang module    │
├────────────────────────────────────────────────────────────────┤
│  C ABI  (backend/src/mobile/agent_api.h — mc_engine_*)         │
├────────────────────────────────────────────────────────────────┤
│  libminiclaw_core — the engine                                 │
│   • ReAct loop, fiber/scheduler (stackful coroutines + libuv)  │
│   • Tools: exec (sandboxed shell on mobile), bash (BusyBox     │
│     on Windows), files, web_search/web_fetch, gmail, cron,     │
│     spawn (subagents), memory_search                           │
│   • Plugins: JS tools in workspace/plugins/ (QuickJS-ng) +     │
│     skills from workspace/skills/ (system-prompt injection)    │
│   • Local models: MNN inference (Qwen3-1.7B, BGE-M3)           │
│   • Memory: 3-stage distillation, Faiss + Lucene++/SQLite      │
└────────────────────────────────────────────────────────────────┘
```

Built with C++20, `libuv` (event loop), `libcurl` (async I/O), `libfiber`
(stackful coroutines — the agent suspends/resumes around every network call
and tool). Every frontend drives the engine through the same C ABI
(`mc_engine_*`): mobile links `libminiclaw_core` in-process, while the
desktop sidecar (`miniclaw`) is a thin host process that links the
same shared library and serves the classic HTTP/SSE API on `localhost:9000`
(`/api/health`, `/api/chat`, `/api/shutdown`, `/v1/chat/completions`). The
mobile build compiles the core with `-DMC_MOBILE`, which swaps the host
shell for `mobile_shell`, enables the path sandbox, and keeps the HTTP
server out of the library.

## Platforms & status

| Platform | Form | Status |
|---|---|---|
| Windows / Linux / macOS | Tauri desktop app (React UI) | ✅ working |
| Android | **Native** Kotlin app, engine in-process | ✅ working (debug) |
| iOS | Native SwiftUI app over the same C ABI | ✅ working — Slack-style topics + local models |

## Roadmap

1. **Proactive agent** *(the big one)* — scheduled jobs (morning digest,
   email triage, travel watch) that survive process death: WorkManager /
   foreground service on Android, Background Tasks on iOS. The engine's
   `cron` tool exists; what's missing is OS-level wake-up and a job model
   designed for "10–30 seconds of process life per wakeup" (state lives in
   the workspace, not RAM).
2. **Real UX** — expandable tool traces, a visible memory panel ("what it
   knows about you", editable), job status. The learning loop only compounds
   if users can *see* it.
3. **Trust features** — Keystore-backed token storage, one-tap memory
   export/backup (move-to-new-phone = copy the workspace), permission
   explanations.
4. **On-device small models** — ✅ MNN inference on mobile: Qwen3-1.7B chat
   + BGE-M3 embeddings run in-process (`docs/LOCAL_MODELS.md`). Remaining:
   on-device distillation by default, model auto-update, larger-model
   quality pass.

## Quick start

### Desktop (Tauri)

```bash
cd backend  && ./tools/setup_deps.sh && ./tools/build.sh
./tools/copy_deps.sh        # bundle miniclaw sidecar + libminiclaw_core DLLs
cd frontend && npm install && npm run tauri dev
```

The desktop app spawns `miniclaw` as a Tauri sidecar; it links
`libminiclaw_core` (the same shared library mobile embeds) and serves the
agent over HTTP/SSE on `localhost:9000`. Port override: `MC_PORT` env var or
`server.port` in `~/.miniclaw/config.yaml`.

### Android (native app)

```bash
cd backend && export ANDROID_NDK_HOME=... && ./tools/build_android.sh
cd android && ./gradlew assembleDebug        # → app-debug.apk
```

Full instructions: [ANDROID.md](android/ANDROID.md) (build, emulator, troubleshooting).

## Repository map

```
backend/    C++ engine (CMake). src/mobile/ = C ABI + JNI bridge.
            tools/ = build scripts per platform, tests/ = unit + e2e harness.
frontend/   React + Vite UI, Tauri host (desktop path).
android/    Native Android app (Kotlin, zero dependencies).
ios/        Native iOS app (SwiftUI, XcodeGen project.yml).
docs/       Design notes (plugins, local models, memory refinements).
```

## Documentation

- [ANDROID.md](android/ANDROID.md) — building & running the native Android app, emulator guide
- [MOBILE.md](MOBILE.md) — mobile architecture, sandbox, storage, model strategy, testing
- [docs/PLUGINS.md](docs/PLUGINS.md) — writing JS tool plugins (QuickJS-ng)
- [docs/LOCAL_MODELS.md](docs/LOCAL_MODELS.md) — on-device MNN models (Qwen3-1.7B, BGE-M3)
- [DESIGN_DOC.md](DESIGN_DOC.md) — engine architecture (fibers, ReAct, memory)
- [backend/IDENTITY.md](backend/IDENTITY.md) — project principles

---
*miniclaw: your agent, on your phone, in your sandbox.*
