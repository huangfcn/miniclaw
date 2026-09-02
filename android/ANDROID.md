# Building & running the native Android app

The app in `android/` is a dependency-free Kotlin app (framework widgets,
no webview) that embeds the C++ engine **in-process** through two shared
libraries:

| Library | Built from | Purpose |
|---|---|---|
| `libminiclaw_core.so` | `backend/` (CMake, NDK, `-DMC_MOBILE`) | the engine — same C ABI as desktop/Tauri |
| `libminiclaw_jni.so` | `backend/src/mobile/jni_bridge.cpp` | JNI bridge for `com.miniclaw.core.NativeEngine` |

Both land in `android/app/src/main/jniLibs/arm64-v8a/` and are packaged
into the APK under `lib/arm64-v8a/`.

## Prerequisites

| Tool | Version used / required | Notes |
|---|---|---|
| Android NDK | r26+ (verified with r27.3) | r27+ has no `platforms/` dir; jni.h comes from the NDK sysroot automatically |
| Android SDK | platform **35**, build-tools 35.x, platform-tools | AGP 8.7.3 does **not** support compileSdk 37 |
| JDK | **17** | set `JAVA_HOME`. ⚠️ Android Studio's bundled JBR may be too new for Gradle 8.9 (JDK 25 fails with an opaque `What went wrong: 25.x` error) — use a standalone JDK 17 on the CLI, or let Android Studio manage its own toolchain when building in-IDE |
| CMake | ≥ 3.20 | used by the NDK toolchain |

The Gradle wrapper is committed in `android/` (Gradle 8.9), so no separate
Gradle install is needed.

## Step 1 — Build the JNI native code

```bash
cd backend
export ANDROID_NDK_HOME=/path/to/android-ndk-r27d   # or NDK_ROOT / NDK_HOME
./tools/build_android.sh
```

What it does:
1. Configures `backend/build-android/` with the NDK CMake toolchain
   (`arm64-v8a`, API 24, Release, `c++_shared`, SQLite on).
2. Builds **both** targets: `miniclaw_core` and `miniclaw_jni`.
3. Copies both `.so` files into `android/app/src/main/jniLibs/arm64-v8a/`
   (and into the Tauri project's `jniLibs` if it exists).

Outputs (in `backend/build-android/`):
- `libminiclaw_core.so` (~110 MB unstripped; ~30 MB total APK)
- `libminiclaw_jni.so` (~195 KB) — exports the 9
  `Java_com_miniclaw_core_NativeEngine_*` symbols

Re-running after C++ changes is incremental: just re-run the script (or
`cmake --build build-android --target miniclaw_jni --parallel 8` for the
bridge alone), then go to step 2.

## Step 2 — Build the app (APK)

```bash
cd android
export JAVA_HOME=/path/to/jdk17
./gradlew.bat assembleDebug        # Windows; use ./gradlew on Linux/macOS
```

Output: `android/app/build/outputs/apk/debug/app-debug.apk`

### `local.properties` (first build only)

Gradle needs the SDK location in `android/local.properties`. **Backslashes
must be doubled** — Java properties parsing treats `\x` as an escape and
eats single backslashes, which turns `C:\Users\...` into a garbage path
and fails with a confusing `IOException`:

```properties
sdk.dir=C\:\\Users\\GSBAI\\AppData\\Local\\Android\\Sdk
```

(On Linux/macOS plain paths work: `sdk.dir=/home/user/Android/Sdk`.)

## Install & run

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.miniclaw.app/.MainActivity
adb logcat -s miniclaw
```

### Using the app

The app has three tabs (bottom bar) — same UX as the iOS app and the React
frontend's mobile views:

- **Chat** — streaming agent replies with avatars, a per-message activity
  panel (`⚡ status`, `🔧 tool start`, `✓ tool end`), a Thinking… indicator,
  and an empty-state hero. Fixed session id `main`.
- **Status** — engine state card (running / starting / error with the
  start error message), info rows (version, status, ticking uptime, model,
  endpoint, workspace) and the privacy note.
- **Settings** — structured LLM form plus a raw `config.yaml` editor:
  - `conversation.endpoint` — full URL, e.g. `http://10.0.2.2:9000/v1/chat/completions`
    from the emulator (see below) or `http://192.168.1.x:9000/...` from a LAN phone
  - `conversation.model`, `conversation.api_key`
  - `web.brave_api_key` — enables the `web_search` tool (Brave Search API).
    On mobile there are no environment variables, so the key **must** come
    from here (the desktop `BRAVE_API_KEY` env var still works on desktop).
  - All four fields persist in SharedPreferences and are live-applied to the
    running engine via `setString()` (no restart needed).
  - The advanced editor loads/saves the full `config.yaml` through
    `mc_get_config` / `mc_set_config`; the Save button highlights while the
    text differs from the last loaded snapshot.
- The engine starts on app start and stops when backgrounded (lifecycle,
  same as iOS).
- The manifest allows cleartext HTTP for local-network servers.

## Running in the Android Studio emulator

1. **Open `android/` as a project** (File → Open), let Gradle sync.
2. **Device Manager → create an AVD with an ARM 64 system image**
   (e.g. "Google APIs ARM 64 v8a, API 35"). ⚠️ The APK ships
   **arm64-v8a only** — an x86_64 emulator will crash at startup with
   `UnsatisfiedLinkError`.
   - On an x86_64 Windows host, ARM64 images run via **WHPX**: the
     *Hyper-V Platform* optional feature must be enabled
     (`Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-Platform`)
     **and the machine rebooted**. It works but is slow.
   - **Faster iteration alternative:** build the native libs for `x86_64`
     too (edit `ABI` in `backend/tools/build_android.sh`, or run cmake with
     `-DANDROID_ABI=x86_64`), copy the `.so`s into
     `jniLibs/x86_64/`, and use a normal x86_64 image.
3. Select the AVD in the device dropdown → **Run ▶**.

Emulator-specific tips:

- **Reach your PC from the emulator**: `10.0.2.2` aliases the host's
  loopback — point `conversation.endpoint` at
  `http://10.0.2.2:<port>/v1/chat/completions`.
- **Inspect the agent's workspace** (debug builds):
  ```bash
  adb shell run-as com.miniclaw.app ls files/workspace
  adb shell run-as com.miniclaw.app cat files/workspace/MEMORY.md
  ```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Gradle `What went wrong: 25.x` (or similar bare version) | `JAVA_HOME` points at a JDK newer than Gradle 8.9 supports (e.g. Android Studio's JBR). Use JDK 17. |
| Gradle: `IOException` / path looks like `C:UsersGSBAI...` | `local.properties` has single backslashes — double them (`C\\:\\Users\\...`). |
| Gradle: `Failed to find Platform SDK with path: platforms;android-NN` | Install that platform (e.g. `platforms/android-35`) or lower `compileSdk` in `app/build.gradle.kts`. |
| CMake: `CMakeCache.txt directory ... is different than ...` after moving the repo | The build tree remembers its old absolute path. Delete `backend/build-android/CMakeCache.txt` + `CMakeFiles/`, and if FetchContent subbuilds complain, rewrite the old path inside `build-android/_deps/*-subbuild/CMakeCache.txt` (preserves prebuilt deps). |
| `UnsatisfiedLinkError: ...NativeEngine.nativeX` at app start | `libminiclaw_jni.so` missing from the APK or stale — re-run step 1, then step 2. |
| `UnsatisfiedLinkError: miniclaw_core` | `libminiclaw_core.so` missing/ABI mismatch — re-run step 1 (builds `arm64-v8a`). |
| Emulator: app crashes at launch with `UnsatisfiedLinkError` | You're on an x86_64 image — the APK only has arm64-v8a. Use an ARM 64 image (or add an x86_64 build). |
| NDK path mangled by MSYS (`C:\msys64\...` → `/c/msys64/...`) | Set the env var with a Windows-style path in single quotes: `export ANDROID_NDK_HOME='C:\msys64\android-ndk-r27d-windows\android-ndk-r27d'`. |

## Desktop e2e test (no device needed)

The engine path under the JNI is the same C ABI exercised on desktop — see
`MOBILE.md` §6.1 (`backend/test/mock_llm.py` + the `mc_e2e` driver). The
JVM side (thread attaching, global refs, UI marshaling) only runs on a real
device/emulator; a missing JNI symbol shows up there as an
`UnsatisfiedLinkError` naming the exact method.
