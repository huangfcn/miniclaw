// ─────────────────────────────────────────────────────────────────────────────
// JNI bridge for the miniclaw mobile engine.
//
// Exposes the C ABI (agent_api.h) to Java/Kotlin as the class
// com.miniclaw.core.NativeEngine. Built as libminiclaw_jni.so, which links
// against libminiclaw_core.so (the engine). The Android app loads:
//   System.loadLibrary("miniclaw_core");
//   System.loadLibrary("miniclaw_jni");
//
// Threading model (important):
//   - mc_send_message() invokes the event callback from an internal worker
//     thread, NOT the JVM thread. Each callback therefore attaches the
//     current native thread to the JVM on demand and calls back into the
//     Java EventListener. Listeners must be fast and non-blocking; marshal
//     to the main thread in Kotlin (see EngineClient).
//   - The listener object is held as a global ref for the duration of one
//     turn. The trampoline releases the ref on the first terminal event
//     ("done"/"error"); the JniListener struct itself is freed by the
//     engine's turn-cleanup hook (engine_internal.h) after run_turn()
//     returns — the loop can emit "done" after "error", so freeing inside
//     the trampoline would use-after-free on a later callback.
// ─────────────────────────────────────────────────────────────────────────────

#include <jni.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mobile/agent_api.h"
#include "mobile/engine_internal.h"

namespace {

constexpr auto kListenerClass = "com/miniclaw/core/NativeEngine$EventListener";

struct JniListener {
  JavaVM *jvm = nullptr;
  jobject listener = nullptr;  // global ref, owned by this struct
  // The agent loop emits "done" even after an "error" event (loop.hpp), and
  // a thrown turn emits only "error" — so the first terminal event triggers
  // release of the global ref, guarded to make it idempotent. The struct
  // itself is freed by jni_turn_cleanup() (engine_internal.h) once the
  // engine's turn worker thread has finished delivering all events.
  std::atomic<bool> released{false};
};

// Event trampoline: called by the engine on a worker thread.
void on_event_trampoline(void *user, const char *type, const char *content) {
  auto *l = static_cast<JniListener *>(user);
  if (!l || !l->listener) return;

  JNIEnv *env = nullptr;
  bool attached = false;
  jint st = l->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (st == JNI_EDETACHED) {
    if (l->jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    attached = true;
  } else if (st != JNI_OK) {
    return;
  }

  // If a terminal event already fired for this turn, the global ref is gone;
  // ignore any further (redundant) events instead of touching freed state.
  if (l->released.load(std::memory_order_acquire)) {
    if (attached) l->jvm->DetachCurrentThread();
    return;
  }

  jclass cls = env->FindClass(kListenerClass);
  if (cls) {
    jmethodID mid = env->GetMethodID(
        cls, "onEvent", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (mid) {
      jstring jtype = env->NewStringUTF(type ? type : "");
      jstring jcontent = env->NewStringUTF(content ? content : "");
      env->CallVoidMethod(l->listener, mid, jtype, jcontent);
      if (env->ExceptionCheck()) {
        // A throwing listener must not take the engine down.
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      env->DeleteLocalRef(jtype);
      env->DeleteLocalRef(jcontent);
    } else if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  } else if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }

  const bool terminal =
      std::strcmp(type, "done") == 0 || std::strcmp(type, "error") == 0;
  JavaVM *jvm = l->jvm;
  if (terminal && !l->released.exchange(true)) {
    // First terminal event: drop the global ref. `l` outlives this call —
    // run_turn() holds the turn slot (and waits on a CV) until after the
    // terminal event, and mc_send_message() returns before the caller can
    // reuse anything, so no later callback can touch `l`.
    env->DeleteGlobalRef(l->listener);
    l->listener = nullptr;
  }
  if (attached) jvm->DetachCurrentThread();
}

// Called by the engine on the turn worker thread after the turn has fully
// completed — no further callbacks for this listener can arrive.
void jni_turn_cleanup(void *user_data) {
  auto *l = static_cast<JniListener *>(user_data);
  if (!l) return;
  // Normal path: the terminal event already released the global ref and
  // nulled l->listener. Defensive path (terminal event never fired — should
  // not happen): attach on demand so the ref is actually released; the
  // worker thread is detached at this point, so GetEnv alone would fail.
  if (!l->released.exchange(true) && l->listener) {
    JNIEnv *env = nullptr;
    bool attached = false;
    jint st = l->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
      if (l->jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
    }
    if (env && l->listener) env->DeleteGlobalRef(l->listener);
    if (attached) l->jvm->DetachCurrentThread();
  }
  delete l;
}

// ── helpers ────────────────────────────────────────────────────────────────

std::string jstr(JNIEnv *env, jstring s) {
  if (!s) return "";
  const char *c = env->GetStringUTFChars(s, nullptr);
  std::string out(c ? c : "");
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

mc_engine *from_handle(jlong h) {
  return reinterpret_cast<mc_engine *>(static_cast<uintptr_t>(h));
}

jlong to_handle(mc_engine *e) {
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(e));
}

JavaVM *get_jvm(JNIEnv *env) {
  JavaVM *jvm = nullptr;
  env->GetJavaVM(&jvm);
  return jvm;
}

} // namespace

// ── native methods ─────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT jstring JNICALL Java_com_miniclaw_core_NativeEngine_nativeVersion(
    JNIEnv *env, jclass) {
  return env->NewStringUTF(mc_version());
}

JNIEXPORT jstring JNICALL Java_com_miniclaw_core_NativeEngine_nativeLastError(
    JNIEnv *env, jclass) {
  return env->NewStringUTF(mc_last_error());
}

JNIEXPORT jlong JNICALL Java_com_miniclaw_core_NativeEngine_nativeCreate(
    JNIEnv *env, jclass, jstring workspace, jstring config_path) {
  // Register the per-turn cleanup hook used to free JniListener objects.
  mc::g_turn_cleanup.store(&jni_turn_cleanup);

  std::string ws = jstr(env, workspace);
  std::string cfg = jstr(env, config_path);
  mc_engine *e =
      mc_engine_create(ws.empty() ? nullptr : ws.c_str(),
                       cfg.empty() ? nullptr : cfg.c_str());
  if (!e) {
    const char *err = mc_last_error();
    jclass cls = env->FindClass("java/lang/RuntimeException");
    env->ThrowNew(cls, err ? err : "mc_engine_create failed");
  }
  return to_handle(e);
}

JNIEXPORT void JNICALL Java_com_miniclaw_core_NativeEngine_nativeDestroy(
    JNIEnv *, jclass, jlong handle) {
  mc_engine *e = from_handle(handle);
  if (e) mc_engine_destroy(e);
}

JNIEXPORT void JNICALL
Java_com_miniclaw_core_NativeEngine_nativeSendMessage(
    JNIEnv *env, jclass, jlong handle, jstring session_id, jstring message,
    jobject listener) {
  mc_engine *e = from_handle(handle);
  if (!e || !listener) return;

  auto *l = new JniListener();
  l->jvm = get_jvm(env);
  l->listener = env->NewGlobalRef(listener);

  std::string session = jstr(env, session_id);
  std::string msg = jstr(env, message);
  mc_send_message(e, session.c_str(), msg.c_str(), &on_event_trampoline, l);
}

JNIEXPORT jstring JNICALL Java_com_miniclaw_core_NativeEngine_nativeGetConfig(
    JNIEnv *env, jclass, jlong handle) {
  mc_engine *e = from_handle(handle);
  if (!e) return nullptr;
  char *cfg = mc_get_config(e);
  jstring out = cfg ? env->NewStringUTF(cfg) : nullptr;
  if (cfg) mc_free_string(cfg);
  return out;
}

JNIEXPORT jint JNICALL Java_com_miniclaw_core_NativeEngine_nativeSetConfig(
    JNIEnv *env, jclass, jlong handle, jstring yaml) {
  mc_engine *e = from_handle(handle);
  if (!e) return -1;
  std::string s = jstr(env, yaml);
  return mc_set_config(e, s.c_str());
}

JNIEXPORT jint JNICALL Java_com_miniclaw_core_NativeEngine_nativeSetString(
    JNIEnv *env, jclass, jlong handle, jstring section, jstring key,
    jstring value) {
  mc_engine *e = from_handle(handle);
  if (!e) return -1;
  std::string sec = jstr(env, section);
  std::string k = jstr(env, key);
  std::string v = jstr(env, value);
  return mc_set_string(e, sec.c_str(), k.c_str(), v.c_str());
}

JNIEXPORT jboolean JNICALL Java_com_miniclaw_core_NativeEngine_nativeIsRunning(
    JNIEnv *, jclass, jlong handle) {
  mc_engine *e = from_handle(handle);
  if (!e) return JNI_FALSE;
  return mc_is_running(e) ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
