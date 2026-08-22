package com.miniclaw.core;

/**
 * Thin JNI wrapper over libminiclaw_core.so / libminiclaw_jni.so.
 *
 * <p>Threading: {@link #sendMessage} delivers events on a background worker
 * thread (the engine's turn fiber). Listeners must be fast and non-blocking;
 * marshal to the main thread yourself (see EngineClient in the app module).
 *
 * <p>Lifecycle: call {@link #create} once (e.g. in Activity.onCreate) and
 * {@link #destroy} in onStop/onDestroy. The instance is not thread-safe for
 * config mutation; sendMessage is safe from any thread.
 */
public final class NativeEngine {

    /** Event listener. Invoked on a worker thread — do not touch the UI here. */
    public interface EventListener {
        /**
         * @param type    one of "status", "token", "tool_start", "tool_end",
         *                "error", "done"
         * @param content event payload (may be empty)
         */
        void onEvent(String type, String content);
    }

    static {
        // Core first (the JNI bridge links against it), then the bridge.
        System.loadLibrary("miniclaw_core");
        System.loadLibrary("miniclaw_jni");
    }

    private long handle; // 0 = not created / destroyed

    private NativeEngine(long handle) {
        this.handle = handle;
    }

    /**
     * Create and start the engine.
     *
     * @param workspaceDir app-private data dir (e.g. context.getFilesDir());
     *                     all sessions/memory/config live here
     * @param configPath   optional path to config.yaml; null = <workspace>/config.yaml
     * @throws RuntimeException if the engine fails to start (see lastError())
     */
    public static NativeEngine create(String workspaceDir, String configPath) {
        long h = nativeCreate(workspaceDir, configPath);
        if (h == 0) throw new RuntimeException("engine create failed: " + lastError());
        return new NativeEngine(h);
    }

    /** Human-readable description of the last engine error. */
    public static String lastError() {
        return nativeLastError();
    }

    /** Library version string, e.g. "0.1.0". */
    public static String version() {
        return nativeVersion();
    }

    /**
     * Send a user message. Events stream via the listener (worker thread),
     * ending with "done" (or "error"). Turns are serialized by the engine;
     * calls made while a turn is running are queued.
     */
    public void sendMessage(String sessionId, String message, EventListener listener) {
        requireAlive();
        nativeSendMessage(handle, sessionId, message, listener);
    }

    /** Current config file contents (YAML), or null if none was loaded. */
    public String getConfig() {
        requireAlive();
        return nativeGetConfig(handle);
    }

    /** Write new config YAML and apply it. @return true on success */
    public boolean setConfig(String yaml) {
        requireAlive();
        return nativeSetConfig(handle, yaml) == 0;
    }

    /** Set a single string setting (section/key/value). @return true on success */
    public boolean setString(String section, String key, String value) {
        requireAlive();
        return nativeSetString(handle, section, key, value) == 0;
    }

    /** Engine status: true = running. */
    public boolean isRunning() {
        return handle != 0 && nativeIsRunning(handle);
    }

    /** Shut down the engine and release native resources. Idempotent. */
    public void destroy() {
        if (handle != 0) {
            long h = handle;
            handle = 0;
            nativeDestroy(h);
        }
    }

    private void requireAlive() {
        if (handle == 0) throw new IllegalStateException("engine not created");
    }

    // ── natives (implemented in libminiclaw_jni.so) ────────────────────────

    private static native String nativeVersion();
    private static native String nativeLastError();
    private static native long nativeCreate(String workspaceDir, String configPath);
    private static native void nativeDestroy(long handle);
    private static native void nativeSendMessage(long handle, String sessionId,
                                                 String message, EventListener listener);
    private static native String nativeGetConfig(long handle);
    private static native int nativeSetConfig(long handle, String yaml);
    private static native int nativeSetString(long handle, String section,
                                              String key, String value);
    private static native boolean nativeIsRunning(long handle);
}
