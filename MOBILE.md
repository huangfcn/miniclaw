# Miniclaw Mobile (Android / iOS)

This document explains how to build miniclaw as a smartphone-native personal
assistant app.

## Architecture

```
┌─────────────────────────────── Phone ───────────────────────────────┐
│  Tauri webview (React UI — chat, status, settings)                  │
│        │  invoke() / events                                          │
│  Rust host (frontend/src-tauri)                                     │
│        │  C ABI (backend/src/mobile/agent_api.h)                     │
│  libminiclaw_core.so / .dylib  ← the existing C++ engine            │
│    • ReAct loop + tools (sandboxed, no shell)                       │
│    • Memory: SQLite FTS5 + Faiss                                    │
│    • Fibers, cron, skills                                            │
└──────────────────────────────────────────────────────────────────────┘
```

Key differences from desktop:

| | Desktop | Mobile |
|---|---|---|
| Engine process | Sidecar binary (`miniclaw.exe`) | Embedded shared library |
| Frontend ↔ engine | HTTP `localhost:9000` SSE | Tauri commands + `agent-event` events |
| Shell/exec tool | Full host shell | Sandboxed POSIX-style shell (`mobile_shell.hpp`, `MC_MOBILE`) |
| File tools | Full filesystem | Sandboxed to app data dir |
| Local models | Any local endpoint | Config-driven endpoints (see below) |

The C++ core is **unchanged** except for:
- `backend/src/mobile/agent_api.{h,cpp}` — thin C ABI wrapper (`mc_engine_*`).
- `backend/src/tools/mobile_sandbox.hpp` + checks in `file.hpp` — path sandbox.
- `backend/src/tools/mobile_shell.hpp` — sandboxed shell for the `mobile_shell`
  tool (see below).
- `backend/src/agent.cpp` — `exec` replaced by `mobile_shell` when `MC_MOBILE`
  is set.
- `backend/CMakeLists.txt` — new `miniclaw_core` shared library target.

### The mobile shell (`mobile_shell` tool)

On mobile the agent cannot run arbitrary host commands, but it still needs to
inspect and edit its memory/workspace. `mobile_shell` provides a small, fully
sandboxed POSIX-style shell implemented in C++ (no fork/exec): `ls`, `cat`,
`head`, `tail`, `wc`, `grep`, `find`, `sort`, `uniq`, `cut`, `echo`, `pwd`,
`mkdir`, `touch`, `cp`, `mv`, `rm`, `basename`, `dirname`, `which`, `date` —
with pipelines (`|`) and quotes. Every path is resolved and checked against
the workspace sandbox (including `..` escapes), so the agent can only touch
its own files.

---

## 1. Build the C++ core for Android

Prereqs: Android NDK (r26+), Android SDK, CMake ≥ 3.20.

```bash
cd backend
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/26.1.10909125   # your path
./tools/build_deps_android.sh          # libcurl, OpenSSL, zlib, OpenBLAS
./tools/build_android.sh               # builds miniclaw + miniclaw_core
```

`build_android.sh` produces (in `build-android/`):
- `libminiclaw_core.so` ← the library Tauri links against
- `miniclaw` (standalone binary, optional for adb debugging)

Copy the library into the Tauri Android project (Tauri merges `jniLibs`
automatically):

```bash
mkdir -p ../frontend/src-tauri/android/app/src/main/jniLibs/arm64-v8a
cp build-android/libminiclaw_core.so \
   ../frontend/src-tauri/android/app/src/main/jniLibs/arm64-v8a/
```

(Add `armeabi-v7a` / `x86_64` the same way if you need those ABIs — most
modern phones only need `arm64-v8a`.)

### iOS (on a Mac)

```bash
cd backend
cmake -B build-ios \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/tools/ios-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SQLITE=ON
cmake --build build-ios --target miniclaw_core -j8
```

(See `tools/build_ios.sh` once added — the Android script is the template;
iOS additionally needs faiss/openssl built for `arm64-apple-ios15.0`. The
existing scripts under `tools/` already handle most of this.)

Then in Xcode (`frontend/src-tauri/ios/App.xcproj`):
1. Drag `libminiclaw_core.dylib` into the project (Copy Items If Needed).
2. Target → General → Frameworks, Libraries: add it under **Embed & Sign**.
3. Add `-l"miniclaw_core"` is implicit via the dylib; ensure the app's
   `Runpath Search Paths` include `@executable_path/Frameworks`.

---

## 2. Initialize the Tauri mobile projects

One-time, on a machine with the Android SDK (and Xcode for iOS):

```bash
cd frontend
npm install @tauri-apps/cli

# Android
npx tauri android init \
  --app-name miniclaw \
  --app-id com.miniclaw.app \
  --org "miniclaw"

# iOS (macOS only)
npx tauri ios init \
  --app-name miniclaw \
  --app-id com.miniclaw.app \
  --org-identifier MINICLAW
```

This generates `src-tauri/android/` and `src-tauri/ios/`. Commit them.

### Android permissions (add to `android/app/src/main/AndroidManifest.xml`)

```xml
<uses-permission android:name="android.permission.INTERNET" />
<!-- optional: keep agent alive for background distillation/cron -->
<uses-permission android:name="android.permission.WAKE_LOCK" />
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
```

Tauri requests `INTERNET` by default; add the others as features land.

---

## 3. Build & run

### Android

```bash
cd frontend
npx tauri android build --debug        # or: npx tauri android dev
adb install -r src-tauri/android/app/build/outputs/apk/debug/app-debug.apk
```

### iOS

```bash
cd frontend
npx tauri ios build                    # opens Xcode / produces .app
npx tauri ios dev                      # run on a connected device
```

---

## 4. Where the app stores data

On mobile the engine workspace is the app's private data dir:

- **Android**: `/data/data/com.miniclaw.app/files/workspace` (i.e. `context.filesDir`)
- **iOS**: `~/Library/Application Support/.../workspace` (sandboxed)

`config.yaml` sits next to it. The file tools can only touch paths inside
this workspace; everything else is rejected with
`"Error: path is outside the app sandbox"`.

### Storage strategy (Android has no general writable filesystem)

Android's `assets/` folder is packed into the APK and is **read-only at
runtime** — you cannot write config, memory files, or indexes there. So the
app follows the standard Android storage rules:

| Data | Location | Why |
|---|---|---|
| `config.yaml`, workspace (memory/, sessions/, skills/, index/) | **Internal storage** (`context.filesDir` via Tauri `app_data_dir`) | Private to the app, no permissions needed, survives low-space clears, deleted on uninstall — exactly what a personal assistant wants |
| Bootstrap files (`AGENTS.md`, `SOUL.md`, `USER.md`, `TOOLS.md`, `IDENTITY.md`) + default config | **Generated in C++ on first launch** into the workspace | No assets to copy: the mobile shim (`seed_bootstrap_files` in `backend/src/mobile/agent_api.cpp`) writes them only if missing, so the user/agent can edit them later. Same code path on iOS |
| Temporary scratch (if ever needed) | `context.cacheDir` | OS may clear it — never put memory state here |
| Large binaries (future on-device GGUF models) | **External app-specific storage** (`context.getExternalFilesDir(...)`) | Still app-private and permission-free, but lives on shared media so multi-GB model files don't pressure the internal partition. Copy from assets/internal on first download, then load by absolute path |
| Shared/MediaStore storage | **Not used** | Would require extra permissions and expose private assistant data to other apps — against the privacy goal |

If you later want richer bundled templates (long prompts, many skills) edited
in Android Studio instead of C++ string constants, the standard pattern is:
put them under `src/main/assets/workspace/` and copy them into `filesDir`
once at first launch (Kotlin, before engine init):

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

---

## 5. Local models & privacy strategy

The config already supports three independent endpoints:

```yaml
llm:
  base_url: "https://api.openai.com/v1"      # conversation (remote, quality)
  api_key: "sk-..."
  model: "gpt-4o-mini"

distillation:                                  # private background work
  enabled: true
  base_url: "http://127.0.0.1:8080/v1"        # local OpenAI-compatible server
  model: "qwen2.5-7b-instruct-q4"             # summarization / memory distill

embeddings:
  base_url: "http://127.0.0.1:8080/v1"        # local embeddings
  model: "nomic-embed-text-v1.5"
```

On a phone, `127.0.0.1` endpoints mean **in-process** or **on-device**
model servers. Two supported paths:

1. **First phase (now)**: point `distillation`/`embeddings` at any
   OpenAI-compatible server you run (home server, LAN, or a later
   in-app llama.cpp service). Conversation stays on the remote API.

2. **Second phase (planned)**: embed llama.cpp inside `miniclaw_core`
   (GGUF models shipped as app assets or downloaded on first launch),
   exposing an in-process OpenAI-compatible endpoint on a local port —
   then all private background work runs fully on-device with zero
   network egress. The C ABI (`mc_engine_*`) already isolates this:
   model loading can be added behind `mc_set_config` without touching the
   Rust or frontend layers.

No root, no adb, no shell — everything stays inside the normal mobile
sandbox and platform APIs.

---

## 6. Testing the mobile shell

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

### 6.1 Desktop end-to-end test (C ABI + mock LLM)

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

---

## 7. Troubleshooting

| Symptom | Fix |
|---|---|
| `UnsatisfiedLinkError: miniclaw_core` | `libminiclaw_core.so` missing from `jniLibs/<abi>/`, or ABI mismatch (build for `arm64-v8a`). |
| Engine init failed on app start | Check logcat for `[mobile] engine init failed`; usually a missing dependency lib (curl/ssl) — rebuild deps with `build_deps_android.sh`. |
| Tools fail with "outside the app sandbox" | Expected: file tools are confined to the workspace. Adjust `memory.workspace` in config only if you know what you're doing. |
| Desktop build broken after changes | Desktop is untouched by design: `cargo tauri dev` / `npm run tauri dev` as before. |
