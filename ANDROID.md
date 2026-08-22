# Building the Android App (native, no webview)

The native Android app lives in `android/`. It embeds the C++ engine
in-process through two shared libraries:

| Library | Built from | Purpose |
|---|---|---|
| `libminiclaw_core.so` | `backend/` (CMake, NDK) | the engine (same C ABI as desktop/Tauri) |
| `libminiclaw_jni.so` | `backend/src/mobile/jni_bridge.cpp` | JNI bridge for `com.miniclaw.core.NativeEngine` |

Both end up in `android/app/src/main/jniLibs/arm64-v8a/` and are packaged
into the APK under `lib/arm64-v8a/`.

## Prerequisites

| Tool | Version used / required | Notes |
|---|---|---|
| Android NDK | r27d (r26+ works) | r27+ has no `platforms/` dir; jni.h comes from the NDK sysroot automatically |
| Android SDK | platform **35**, build-tools 35.x, platform-tools | AGP 8.7.3 does **not** support compileSdk 37 |
| JDK | 17 | set `JAVA_HOME` |
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
- `libminiclaw_core.so` (~110 MB unstripped)
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

The app stores its workspace in the app's `filesDir/workspace` (same layout
as the Tauri path). In the settings dialog (⚙) set `conversation.endpoint`
to a full URL, e.g. `http://192.168.1.10:9000/v1/chat/completions` — the
manifest allows cleartext HTTP for local-network servers.

### Emulator note (Windows with Hyper-V)

If `emulator` reports *"Virtualization extension is not supported"* while
Hyper-V is running, the Windows **Hypervisor Platform** feature must be
enabled (`Get-WindowsOptionalFeature -Online -FeatureName
Microsoft-Hyper-V-Platform`) **and the machine rebooted**. Without it, x86_64
system images cannot run at all.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Gradle: `IOException` / path looks like `C:UsersGSBAI...` | `local.properties` has single backslashes — double them (`C\\:\\Users\\...`). |
| Gradle: `Failed to find Platform SDK with path: platforms;android-NN` | Install that platform (e.g. `platforms/android-35`) or lower `compileSdk` in `app/build.gradle.kts`. |
| `UnsatisfiedLinkError: ...NativeEngine.nativeX` at app start | `libminiclaw_jni.so` missing from the APK or stale — re-run step 1, then step 2. |
| `UnsatisfiedLinkError: miniclaw_core` | `libminiclaw_core.so` missing/ABI mismatch — re-run step 1 (builds `arm64-v8a`). |
| NDK path mangled by MSYS (`C:\msys64\...` → `/c/msys64/...`) | Set the env var with a Windows-style path in single quotes: `export ANDROID_NDK_HOME='C:\msys64\android-ndk-r27d-windows\android-ndk-r27d'`. |

## Desktop e2e test (no device needed)

The engine path under the JNI is the same C ABI exercised on desktop — see
`MOBILE.md` §6.1 (`backend/test/mock_llm.py` + `run_e2e_scenario.sh`). The
JVM side (thread attaching, global refs, UI marshaling) only runs on a real
device/emulator; a missing JNI symbol shows up there as an
`UnsatisfiedLinkError` naming the exact method.
