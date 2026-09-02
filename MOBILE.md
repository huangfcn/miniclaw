# Miniclaw Mobile (Android / iOS)

## 1. Vision & product model

miniclaw on a phone is **a personal agent that lives in the app's own
sandbox** — not a client to a desktop gateway, not a cloud service. The
motivation:

- Desktop/cloud agents (OpenClaw & co.) are powerful but ask the user to
  solve a *deployment problem* first: a machine that stays on, a service
  that stays up, a setup most people never do.
- The phone is the one computer a normal person already carries, always has
  on, and already trusts with their identity (email, banking, calendar).
  An agent shipped *as an app* is pre-deployed by definition.

The target jobs are **life-shaped and network-shaped** — they need outbound
HTTP, not filesystem or shell power:

| Job | Tools involved |
|---|---|
| Daily news / finance digest | `web_search`, `web_fetch` + `cron` |
| Email triage | `gmail` (OAuth token kept in the sandbox) |
| Travel & hotel research | `web_search`, `web_fetch` |
| "Knows you" continuity | memory distillation + hybrid search |

**Model strategy** — three independent, config-driven endpoints
(`config.yaml`):

| Section | Role | Phase 1 (now) | Phase 2 (planned) |
|---|---|---|---|
| `conversation` | the agent's reasoning | strong API model | strong API model |
| `memory` | distillation (summarization into L2/L3) | cheap API model | **small on-device model** |
| `embedding` | RAG vectorizer | API or local endpoint | **small on-device model** |

Phase 2 runs the two *small* models on-device (in-process llama.cpp behind
the same C ABI, GGUF models in app-specific external storage) so private
background work has zero network egress. Conversation quality stays on the
API — a phone-sized model is not yet good enough for ReAct reasoning, and
distillation quality is what the "knows you" loop depends on, so don't rush
that swap.

**The learning loop.** Every conversation feeds 3-stage distillation:
`sessions/*.jsonl` → `memory/YYYY-MM-DD.md` (daily) → `MEMORY.md` /
`USER.md` (permanent). Retrieval is hybrid Faiss + Lucene++ with reciprocal
rank fusion and temporal decay. The brain is plain files in the workspace —
readable, editable, exportable. That inspectability is the trust story no
cloud assistant can match: the user can open `MEMORY.md` and see exactly
what the agent believes about them.

## 2. Architecture

The mobile app is **native on each platform** — no webview, no localhost
HTTP; the C++ engine runs **in-process**. Each platform has its own app and
its own thin bridge to the C ABI; only the engine (and the sandbox rules)
is shared:

```
┌─────────────────────────────── Phone ───────────────────────────────┐
│  Native app — one per platform, each its own UI & codebase          │
│    • Android: Kotlin UI — 3 tabs (Chat/Status/Settings) [shipped]   │
│    • iOS:     SwiftUI — 3 tabs (Chat/Status/Settings)  [shipped]    │
│        │                                                            │
│  Thin bridge to the C ABI (per platform)                            │
│    • Android: libminiclaw_jni.so (JNI, jni_bridge.cpp)              │
│    • iOS:     Clang module over agent_api.h (module.modulemap)      │
│        │  C ABI (backend/src/mobile/agent_api.h — mc_engine_*)      │
│  libminiclaw_core.so / .dylib  ← the C++ engine                     │
│    • ReAct loop + tools (sandboxed, no shell)                       │
│    • Memory: 3-stage distillation, Faiss + Lucene++ hybrid search   │
│    • Fibers, cron, skills                                           │
└─────────────────────────────── Phone ───────────────────────────────┘
```

Desktop uses the same `libminiclaw_core` as a sidecar binary behind HTTP/SSE;
on mobile it is embedded as a shared library. The iOS app is **its own app**
— it does not need to match Android's UI or structure; it only shares the
C ABI and the engine.

Key differences from desktop:

| | Desktop | Mobile |
|---|---|---|
| Engine process | Sidecar `miniclaw` (thin host over the C ABI, links `libminiclaw_core`) | Embedded shared library |
| Frontend ↔ engine | HTTP `localhost:9000` SSE | JNI (Android) / Clang module (iOS), in-process |
| Shell/exec tool | Full host shell | Sandboxed POSIX-style shell (`mobile_shell.hpp`, `MC_MOBILE`) |
| File tools | Full filesystem | Sandboxed to app data dir |
| Env vars | available | **none** — all secrets/keys via config (`mc_set_string`) |

The C++ core is **unchanged** except for:
- `backend/src/mobile/agent_api.{h,cpp}` — thin C ABI wrapper (`mc_engine_*`),
  incl. first-launch workspace seeding (`seed_bootstrap_files`,
  `kDefaultConfigYaml`).
- `backend/src/tools/mobile_sandbox.hpp` + checks in `file.hpp` — path sandbox.
- `backend/src/tools/mobile_shell.hpp` — sandboxed shell (below).
- `backend/src/agent.cpp` — `exec` replaced by `mobile_shell` when
  `MC_MOBILE` is set.
- `backend/CMakeLists.txt` — `miniclaw_core` shared library target.

### Android implementation (current)

The JNI bridge
(`jni_bridge.cpp` → `libminiclaw_jni.so`) implements the 9
`Java_com_miniclaw_core_NativeEngine_*` methods over the C ABI; engine
callbacks arrive on worker threads that the bridge attaches to the JVM.
`EngineClient.kt` hops events to the UI thread; `MainActivity.kt` renders
the three-tab UX (framework views only, no dependencies):
- **Chat** — streaming bubbles with avatars, per-message activity panel
  (`⚡`/`🔧`/`✓` lines), Thinking… indicator, empty-state hero;
- **Status** — engine state card, info rows (version/status/uptime/model/
  endpoint/workspace) with a ticking uptime, privacy note, start-error card;
- **Settings** — structured LLM form (`conversation.*` +
  `web.brave_api_key`, persisted in SharedPreferences, pushed live via
  `setString()`) plus a raw `config.yaml` editor (dirty-tracked,
  `mc_get_config`/`mc_set_config`).

**Full build & run instructions: [ANDROID.md](android/ANDROID.md)** — short version:

```bash
cd backend && export ANDROID_NDK_HOME=... && ./tools/build_android.sh  # both .so → jniLibs
cd android && ./gradlew assembleDebug                                   # → app-debug.apk
```

### iOS implementation (current)

No bridge library: Swift imports the C ABI directly. `ios/Miniclaw/FFI/module.modulemap`
exposes `backend/src/mobile/agent_api.h` as module `MiniclawCore` (registered via
`-fmodule-map-file` in `ios/project.yml`), and `EngineClient.swift` wraps it exactly
like Android's `EngineClient.kt` — main-thread API, blocking create/destroy on a
private serial queue, worker-thread events marshalled to main through the C callback.
`MiniclawApp.swift` drives the lifecycle from `scenePhase` (active → start,
background → destroy). The Xcode project is generated by XcodeGen from
`ios/project.yml`; a run-script phase embeds and signs the engine dylib.

**Full build & run instructions: [ios/README.md](ios/README.md)** — short version:

```bash
bash backend/tools/build_ios.sh          # → backend/build-ios/libminiclaw_core.dylib
cd ios && xcodegen generate && open Miniclaw.xcodeproj
```

### The mobile shell (`mobile_shell` tool)

On mobile the agent cannot run arbitrary host commands, but it still needs
to inspect and edit its memory/workspace. `mobile_shell` is a small, fully
sandboxed POSIX-style shell implemented in C++ (**no fork/exec**): `ls`,
`cat`, `head`, `tail`, `wc`, `grep`, `find`, `sort`, `uniq`, `cut`, `echo`,
`pwd`, `mkdir`, `touch`, `cp`, `mv`, `rm`, `basename`, `dirname`, `which`,
`date` — with pipelines (`|`) and quotes. Every path is resolved and checked
against the workspace sandbox (including `..` escapes), so the agent can
only touch its own files.

### Sandbox & permissions

The confinement is layered, strongest first:

1. **OS** — the app runs as its own Linux UID; with no storage permissions
   granted, the kernel blocks all file access outside the app's private dir.
2. **Engine** — `MC_MOBILE_PATH_CHECK` in every file tool; `mobile_shell`
   builtin-only, no process spawning.
3. **Manifest** — `INTERNET` only (+ cleartext for local-network LLM
   servers). No contacts, no location, no other-app data.

Consequence: the agent is a full-power agent *inside its own home
directory*. It can't be pointed at other apps' data or the host system —
by construction, not by policy.

## 3. Where the app stores data

On mobile the engine workspace is the app's private data dir:

- **Android**: `/data/data/com.miniclaw.app/files/workspace` (i.e. `context.filesDir`)
- **iOS**: the app's sandboxed container (Documents/Application Support)

`config.yaml` sits next to it. The file tools can only touch paths inside
this workspace; everything else is rejected with
`"Error: path is outside the app sandbox"`.

### Storage strategy (Android has no general writable filesystem)

Android's `assets/` folder is packed into the APK and is **read-only at
runtime** — you cannot write config, memory files, or indexes there. So the
app follows the standard Android storage rules:

| Data | Location | Why |
|---|---|---|
| `config.yaml`, workspace (memory/, sessions/, skills/, index/) | **Internal storage** (`context.filesDir`) | Private to the app, no permissions needed, deleted on uninstall — exactly what a personal assistant wants |
| Bootstrap files (`AGENTS.md`, `SOUL.md`, `USER.md`, `TOOLS.md`, `IDENTITY.md`) + default config | **Generated in C++ on first launch** into the workspace | No assets to copy: `seed_bootstrap_files` in `agent_api.cpp` writes them only if missing, so the user/agent can edit them later. Same code path on iOS |
| Temporary scratch (if ever needed) | `context.cacheDir` | OS may clear it — never put memory state here |
| Large binaries (future on-device GGUF models) | **External app-specific storage** (`context.getExternalFilesDir(...)`) | Still app-private and permission-free, but lives on shared media so multi-GB model files don't pressure the internal partition |
| Shared/MediaStore storage | **Not used** | Would require extra permissions and expose private assistant data to other apps — against the privacy goal |

If you later want richer bundled templates (long prompts, many skills)
edited in Android Studio instead of C++ string constants, the standard
pattern is: put them under `src/main/assets/workspace/` and copy them into
`filesDir` once at first launch (Kotlin, before engine init):

```kotlin
fun Context.copyAssetDir(assetDir: String, dest: File) {
    assets.list(assetDir)?.forEach { name ->
        val assetPath = "$assetDir/$name"
        val out = File(dest, name)
        if (assets.list(assetPath) != null) copyAssetDir(assetPath, out)
        else assets.open(assetPath).use { input ->
            FileOutputStream(out).use { input.copyTo(it) }
        }
    }
}
// first launch only:
copyAssetDir("workspace", File(filesDir, "workspace"))
```

The C++ seeding already covers the default case, so this is optional.

## 4. Building

### Android (native app) — see [ANDROID.md](android/ANDROID.md)

Prereqs: NDK r26+ (verified r27.3), SDK platform 35, **JDK 17** (note:
Android Studio's bundled JBR may be too new for the committed Gradle 8.9),
CMake ≥ 3.20.

```bash
cd backend
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/27.3.13750724   # your path
./tools/build_deps_android.sh          # libcurl, OpenSSL, zlib, OpenBLAS (one-time)
./tools/build_android.sh               # builds miniclaw_core + miniclaw_jni → jniLibs

cd android
export JAVA_HOME=/path/to/jdk17
./gradlew assembleDebug                # → app/build/outputs/apk/debug/app-debug.apk
```

The APK ships `lib/arm64-v8a/` only — **emulators need an ARM 64 system
image** (or add an `x86_64` build for fast x86 iteration; details and the
WHPX/Hyper-V notes are in `android/ANDROID.md`).

### Manifest permissions for background features (roadmap)

The manifest currently requests `INTERNET` only. When the proactive-agent
work lands (§6), add:

```xml
<uses-permission android:name="android.permission.WAKE_LOCK" />
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
<!-- foregroundService + FOREGROUND_SERVICE_DATA_SYNC for job execution -->
```

### iOS (on a Mac)

The engine exposes a plain C ABI. The native SwiftUI app (`ios/`, see
[ios/README.md](ios/README.md)) imports `agent_api.h` directly through a
Clang module — no bridge library (unlike Android's JNI) and no webview:

```bash
# 1. engine dylib for arm64-apple-ios
bash backend/tools/build_ios.sh

# 2. Xcode project (one-time: brew install xcodegen)
cd ios && xcodegen generate && open Miniclaw.xcodeproj
```

The app target links `backend/build-ios/libminiclaw_core.dylib` and a build
phase embeds + signs it into the bundle (`@executable_path/Frameworks`
runpath). It fails fast with a hint if the dylib is missing.

The script configures with `-DCMAKE_SYSTEM_NAME=iOS` (no toolchain file),
`-DUSE_SQLITE=ON`, and Release, then builds `miniclaw_core`. Everything is
resolved inside that single configure:

- **faiss, libuv, uSockets/uWebSockets, yaml-cpp, simdjson, fiber** —
  FetchContent, compiled for `arm64-apple-ios`.
- **libcurl** — FetchContent, built statically against SecureTransport (the
  iPhoneOS SDK does not ship curl; no OpenSSL exists on iOS).
  `CURL_USE_LIBPSL=OFF`: libpsl only serves cookie-domain handling and we
  never use cookies. Deliberate choice to keep libcurl (rather than wrapping
  Network.framework/URLSession): the engine's async HTTP core is a
  libcurl-multi ↔ libuv bridge (`CurlMultiManager`), and a native-framework
  backend would mean a second threading domain + re-derived redirect/
  timeout/streaming semantics on one platform only. Revisit if HTTP/3 or
  TLS 1.3-only endpoints ever become a requirement.
- **zlib, Accelerate (BLAS/LAPACK)** — from the iOS SDK.
- **OpenMP** — not needed: faiss is patched (`faiss-local.patch`) to treat it
  as optional and gets a single-threaded stub `omp.h` (`third-party/omp-stub`).
- **Memory index** — SQLite FTS5 (`-DUSE_SQLITE=ON`), built from the
  amalgamation via FetchContent. This also means **no Boost on iOS at all**:
  Lucene++ (the only non-Windows Boost consumer) is gone, and fiber_pool uses
  the vendored C fiber runtime outside Windows (Boost.Fiber there is a
  Windows-only dependency).
- **Boost** — not needed on iOS. (macOS desktop builds still need
  `brew install boost` unless they also pass `-DUSE_SQLITE=ON`.)

The app mirrors the Android lifecycle: the engine starts when the app
becomes active and is destroyed when it backgrounds; workspace data lives in
`Application Support/miniclaw/`. Three tabs (Chat / Status / Settings) port
the React frontend's mobile UI 1:1, including the raw `config.yaml` editor.

## 5. Testing

### 5.1 Mobile shell unit tests

A functional test suite lives in `backend/test/mobile_shell_test.cpp`
(compile with `-DMC_MOBILE`; it exercises every command, pipelines, and
sandbox-escape rejection):

```bash
cd backend
g++ -std=c++20 -O1 -DMC_MOBILE \
  -Isrc -Iexternal/spdlog/include -Ithird-party/fiber/include-win64 \
  -Ibuild/_deps/libuv-src/include -Ibuild/_deps/uwebsockets-src/src \
  -Ibuild/_deps/usockets-src/src -Ibuild/_deps/yaml-cpp-src/include \
  -Ibuild/_deps/simdjson-src/include -Ibuild/_deps/simdjson-src/src \
  -Ithird-party/croncpp \
  test/mobile_shell_test.cpp build/_deps/yaml-cpp-build/libyaml-cpp.a \
  -o build/mc_shell_test && ./build/mc_shell_test
```

Expected output ends with `ALL TESTS PASSED`.

### 5.2 Desktop end-to-end test (C ABI + mock LLM)

`backend/test/mobile_e2e_test.cpp` drives the real mobile entry points
(`mc_engine_create` → `mc_send_message`) on desktop, proving the full path:
C ABI → agent loop → LLM tool call → `MobileShellTool` → sandboxed shell.
It statically links the core sources with `MC_MOBILE`, so no Android/iOS
device is needed.

Build (adds an optional CMake target):

```bash
cd backend
cmake -B build -DMC_E2E_TEST=ON
cmake --build build --target mc_e2e
```

Run it against a mock OpenAI-compatible server. The mock
(`backend/test/mock_llm.py`) serves `/v1/chat/completions` (returns one
`exec` tool call, then a final answer) and `/v1/embeddings`. The command the
mock issues is read per-request from `C:/tmp/mc_mock_cmd.txt`, so each
scenario only needs to rewrite that file:

```bash
python backend/test/mock_llm.py &          # listens on 127.0.0.1:9876

# happy path: mock issues `ls | sort`, expect the workspace listing
printf 'ls | sort' > /c/tmp/mc_mock_cmd.txt

# sandbox escapes: mock issues a path outside the workspace,
# expect "Error: path is outside the app sandbox"
printf 'cat /etc/passwd' > /c/tmp/mc_mock_cmd.txt
```

The driver expects a fresh workspace with `config.yaml` pointing at the mock:

```yaml
conversation:
  provider: openai
  model: mock-model
  endpoint: "http://127.0.0.1:9876/v1/chat/completions"   # full URL, used verbatim
embedding:
  endpoint: "http://127.0.0.1:9876/v1/embeddings"
  model: mock-embed
  dimension: 1536
```

```bash
./build/mc_e2e.exe C:/tmp/mc_e2e_ws "list files"
# → [event] tool_start: exec: {"command": "ls | sort"}
# → [event] tool_end:  (full multi-line listing)
# → E2E PASS: exec ran via mobile shell
```

Notes:
- `conversation.endpoint` is used **verbatim** — it must be the complete URL.
- Use a fresh workspace per scenario: session state persists, and an old
  tool result in history makes the mock skip issuing a new tool call.
- Mobile mode does not start the local HTTP server (the engine is driven
  through the C ABI only), so missing `http://127.0.0.1:PORT` is expected.
- Tool output is multi-line; don't filter driver output with
  `grep '^\[event\]'` or you will drop all but the first line.

### 5.3 On-device checks

The JVM side (thread attaching, global refs, main-thread marshaling) only
runs on a real device/emulator:

```bash
adb logcat -s miniclaw                      # engine logs
adb shell run-as com.miniclaw.app ls files/workspace   # inspect the brain
```

A missing JNI symbol shows up as `UnsatisfiedLinkError` naming the exact
method.

## 6. Roadmap (mobile)

1. **Proactive agent** *(the big one)* — scheduled jobs (morning digest,
   email triage, travel watch) that survive process death: WorkManager /
   foreground service on Android, Background Tasks on iOS. The engine's
   `cron` tool exists; what's missing is OS-level wake-up and a job model
   designed for "10–30 seconds of process life per wakeup" — state lives in
   the workspace, not RAM: cold start → read workspace → do work → write
   memory → notify → die.
2. **Real UX** — expandable tool traces, a visible memory panel ("what it
   knows about you", editable), job status. The learning loop only compounds
   if users can *see* it.
3. **Trust features** — Keystore-backed token storage (Gmail OAuth),
   one-tap memory export/backup (move-to-new-phone = copy the workspace),
   permission explanations.
4. **iOS** — same core; Obj-C++ shim + app shell.
5. **On-device small models** — distillation & embeddings locally (phase 2
   of §1's model strategy).

## 7. Troubleshooting

| Symptom | Fix |
|---|---|
| `UnsatisfiedLinkError: miniclaw_core` | `libminiclaw_core.so` missing from `jniLibs/<abi>/`, or ABI mismatch (build for `arm64-v8a`). |
| `UnsatisfiedLinkError: ...NativeEngine.nativeX` | `libminiclaw_jni.so` missing or built against a different `NativeEngine.java` — rebuild with `build_android.sh`. |
| `web_search`: "BRAVE_API_KEY not configured" on mobile | Set it in the app's settings dialog (⚙) — Android processes have no env vars; the key is read from config (`web.brave_api_key`). Desktop: `BRAVE_API_KEY` env var still works. |
| Gradle: `What went wrong: 25.x` (bare version) | `JAVA_HOME` points at a JDK newer than Gradle 8.9 supports — use JDK 17. |
| CMake: `CMakeCache.txt directory ... is different` after moving the repo | Build tree remembers its old absolute path — delete `build-android/CMakeCache.txt` + `CMakeFiles/` (see `android/ANDROID.md` for the FetchContent subbuild fix). |
| Engine init failed on app start | Check logcat for `[mobile] engine init failed`; usually a missing dependency lib (curl/ssl) — rebuild deps with `build_deps_android.sh`. |
| Tools fail with "outside the app sandbox" | Expected: file tools are confined to the workspace. Adjust `memory.workspace` in config only if you know what you're doing. |
| Desktop build broken after changes | Desktop is untouched by design: `cargo tauri dev` / `npm run tauri dev` as before. |
