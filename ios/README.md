# miniclaw iOS (native SwiftUI)

Pure native iOS app — no Tauri, no webview. Mirrors the Android Kotlin app:
the engine (`libminiclaw_core`) runs **in-process** and is driven through the
C ABI in `backend/src/mobile/agent_api.h`. The difference from Android:
there is no JNI bridge library; Swift imports the C header directly via a
Clang module (`Miniclaw/FFI/module.modulemap`).

## Layout

```
ios/
├── project.yml                  # XcodeGen spec (source of truth)
└── Miniclaw/
    ├── Info.plist
    ├── FFI/module.modulemap     # exposes agent_api.h as module MiniclawCore
    ├── App/
    │   ├── MiniclawApp.swift    # @main, scenePhase ↔ engine start/stop
    │   └── Theme.swift          # colors 1:1 with the Tauri/React UI
    ├── Engine/
    │   ├── EngineClient.swift   # FFI wrapper (≈ Android EngineClient.kt)
    │   └── Models.swift         # LocalModel catalog (MNN models, download URLs)
    ├── State/
    │   ├── AppState.swift       # per-topic messages, settings, event routing
    │   ├── Topics.swift         # Topic model + TopicStore (Slack channel list)
    │   └── ModelStore.swift     # model downloads + engine load status + toggles
    └── Views/
        ├── RootView.swift       # TabView: Home / Models / Status / Settings
        ├── TopicsListView.swift # Slack-style topic list (pinned/topics, add/delete)
        ├── ChatView.swift       # TopicChatView: per-topic channel view + composer
        ├── LocalModelsView.swift# Models tab: download/load/provider toggles
        ├── StatusView.swift     # port of mobile Monitoring view + details
        └── SettingsView.swift   # LLM form + raw config.yaml editor
```

## Build

```sh
# 0. one-time
brew install xcodegen

# 1. build the engine dylib — pick the variant you run against (or both):
bash backend/tools/build_ios.sh            # device    → backend/build-ios/libminiclaw_core.dylib (arm64)
bash backend/tools/build_ios.sh simulator  # simulator → backend/build-ios-iphonesimulator/... (host arch)

# 2. generate the Xcode project
cd ios && xcodegen generate

# 3. open, select your team (Signing & Capabilities), run on a device or
#    simulator. The "Embed engine dylib" build phase picks the dylib that
#    matches the build platform (EFFECTIVE_PLATFORM_NAME), copies and
#    ad-hoc-signs it into the app bundle; it fails fast with the right
#    build command if the matching variant is missing.
open Miniclaw.xcodeproj
```

The app embeds `libminiclaw_core.dylib` in `Frameworks/` and finds it at
runtime via `@executable_path/Frameworks` (runpath set in project.yml). The
dylib itself links only Apple system libraries — no Homebrew, no Boost, no
libomp (see MOBILE.md for the iOS dependency matrix).

## Behavior

- **Lifecycle** (mirrors Android `onStart`/`onStop`): the engine starts when
  the app becomes active and is destroyed when it goes to the background.
  A create that is still in flight when the app backgrounds is discarded.
- **Topics** (Slack-style): Home shows a channel list — pinned topics on top,
  the rest below. Each topic is one engine session (`Topic.id`), so every
  topic keeps its own conversation history on disk: News Summary, Weather,
  Financial Status, Travelling, Meeting Notes are seeded on first launch; the
  user can pin/unpin (row context menu or chat header) and add custom topics
  ("+" button). Pins and custom topics persist in UserDefaults.
- **Sessions**: a topic's id is passed verbatim as the backend session key —
  `#news` → `sessions/news.jsonl`, `#travel` → `sessions/travel.jsonl` under
  the workspace. The Status tab lists the session files on disk; memory
  distillation runs per session, so each topic's context stays separate.
- **Topic purposes**: `TopicStore` mirrors the topic list to
  `<workspace>/topics.json`; the backend's ContextBuilder reads it and injects
  a "## Current Topic" section into that session's system prompt (e.g. #meetings
  knows it is a work journal — when asked "what do you suggest based on my
  behavior?" it searches memory for past entries, analyzes patterns, then gives
  actionable advice). The channel view also offers one-tap suggested prompts.
- **Chat**: per-topic channel view — streaming tokens, activity panel
  (⚡ status / 🔧 tool_start / ✓ tool_end), Thinking… indicator — same event
  model as the React frontend, delivered on the main thread. The engine
  serializes turns globally (one at a time, FIFO across topics); `AppState`
  keeps a `pendingTopics` queue because C ABI events carry no session id,
  so each event is routed to the topic whose turn is in flight.
- **Settings**: structured LLM fields (endpoint/model/api_key/brave key)
  persisted in UserDefaults and applied live via `mc_set_string`, plus a raw
  `config.yaml` editor via `mc_get_config` / `mc_set_config`.
- **Local models** (Models tab): on-device MNN inference. Qwen3-1.7B is
  downloaded straight from HuggingFace into `models/qwen3-1.7b/` with a
  progress bar; BGE-M3 needs a one-time conversion on a dev machine (the UI
  shows the steps and accepts a custom base URL). "Load in engine" calls
  `mc_local_load`, status polls `mc_local_status`. Per-slot provider toggles
  (`conversation.provider` / `embedding.provider` → `mnn`/`openai`) persist
  in UserDefaults and are re-applied when the engine starts. See
  [docs/LOCAL_MODELS.md](../docs/LOCAL_MODELS.md).
- **Workspace**: `Application Support/miniclaw/` — sessions, memory index,
  config.yaml all live there (created on first launch); downloaded models
  live in `<workspace>/models/`.

## Threading contract

- All `EngineClient` / `AppState` methods are called from the main thread.
- `mc_engine_create` / `mc_engine_destroy` run on a private serial queue
  (they block); results are marshalled back to main.
- Engine events arrive on the engine's worker thread; the C bridge copies
  the strings and hops to main before `onEvent` fires (the callback itself
  stays fast and non-blocking, as required by the ABI).
