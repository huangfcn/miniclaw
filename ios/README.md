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
    │   └── EngineClient.swift   # FFI wrapper (≈ Android EngineClient.kt)
    ├── State/
    │   └── AppState.swift       # messages, settings, event handling
    └── Views/
        ├── RootView.swift       # TabView: Chat / Status / Settings
        ├── ChatView.swift       # port of frontend Chat.tsx
        ├── StatusView.swift     # port of mobile Monitoring view + details
        └── SettingsView.swift   # LLM form + raw config.yaml editor
```

## Build

```sh
# 0. one-time
brew install xcodegen

# 1. build the engine dylib (arm64, device SDK)
bash backend/tools/build_ios.sh
#    → backend/build-ios/libminiclaw_core.dylib

# 2. generate the Xcode project
cd ios && xcodegen generate

# 3. open, select your team (Signing & Capabilities), run on a device or
#    simulator (arm64). The "Embed engine dylib" build phase copies and
#    ad-hoc-signs the dylib into the app bundle; it fails fast with a hint
#    if step 1 was skipped.
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
- **Chat**: fixed session `main`, streaming tokens, activity panel
  (⚡ status / 🔧 tool_start / ✓ tool_end), Thinking… indicator — same event
  model as the React frontend, delivered on the main thread.
- **Settings**: structured LLM fields (endpoint/model/api_key/brave key)
  persisted in UserDefaults and applied live via `mc_set_string`, plus a raw
  `config.yaml` editor via `mc_get_config` / `mc_set_config`.
- **Workspace**: `Application Support/miniclaw/` — sessions, memory index,
  config.yaml all live there (created on first launch).

## Threading contract

- All `EngineClient` / `AppState` methods are called from the main thread.
- `mc_engine_create` / `mc_engine_destroy` run on a private serial queue
  (they block); results are marshalled back to main.
- Engine events arrive on the engine's worker thread; the C bridge copies
  the strings and hops to main before `onEvent` fires (the callback itself
  stays fast and non-blocking, as required by the ABI).
