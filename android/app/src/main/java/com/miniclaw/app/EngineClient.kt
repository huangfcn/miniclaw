package com.miniclaw.app

import android.os.Handler
import android.os.Looper
import com.miniclaw.core.NativeEngine

/**
 * Owns the native engine; marshals worker-thread events onto the main looper.
 *
 * Threading contract (mirrors ios/Miniclaw/Engine/EngineClient.swift):
 *  - all methods are called from the main thread;
 *  - blocking create/destroy run on a background thread, results posted back;
 *  - [UiListener] callbacks always fire on the main thread.
 */
class EngineClient(val workspaceDir: String) {

    interface UiListener {
        fun onToken(text: String)
        fun onToolStart(content: String)
        fun onToolEnd(content: String)
        fun onStatus(text: String)
        fun onError(text: String)
        fun onDone()
    }

    @Volatile var engine: NativeEngine? = null
        private set

    /** True while a create is in flight (or the engine is running). */
    @Volatile var isStarting: Boolean = false
        private set

    @Volatile var startError: String? = null
        private set

    /** Wall-clock ms when the engine last became ready (null if never). */
    @Volatile var readyAt: Long? = null
        private set

    private val main = Handler(Looper.getMainLooper())
    private var listener: UiListener? = null

    /** Fired on the main thread after a start attempt finishes (success or failure). */
    var onEngineState: (() -> Unit)? = null

    fun setUiListener(l: UiListener) {
        listener = l
    }

    /** Start the engine on a background thread (bootstraps the workspace). No-op if already running. */
    fun start() {
        if (isReady() || isStarting) return
        isStarting = true
        startError = null
        Thread {
            try {
                val e = NativeEngine.create(workspaceDir, null)
                engine = e
                readyAt = System.currentTimeMillis()
                main.post {
                    isStarting = false
                    onEngineState?.invoke()
                }
            } catch (t: Throwable) {
                startError = t.message ?: "engine start failed"
                isStarting = false
                main.post {
                    listener?.onError(startError!!)
                    onEngineState?.invoke()
                }
            }
        }.start()
    }

    fun isReady(): Boolean = engine?.isRunning() == true

    fun version(): String = NativeEngine.version()

    /** Current config.yaml contents, or null if none was loaded. Main thread. */
    fun getConfig(): String? = try {
        engine?.getConfig()
    } catch (t: Throwable) {
        null
    }

    /** Write new config YAML and apply it. @return true on success. Main thread. */
    fun setConfig(yaml: String): Boolean = try {
        engine?.setConfig(yaml) ?: false
    } catch (t: Throwable) {
        false
    }

    fun send(sessionId: String, message: String) {
        val e = engine ?: return
        e.sendMessage(sessionId, message, object : NativeEngine.EventListener {
            override fun onEvent(type: String, content: String) {
                when (type) {
                    "token" -> main.post { listener?.onToken(content) }
                    "tool_start" -> main.post { listener?.onToolStart(content) }
                    "tool_end" -> main.post { listener?.onToolEnd(content) }
                    "status" -> main.post { listener?.onStatus(content) }
                    "error" -> main.post { listener?.onError(content) }
                    "done" -> main.post { listener?.onDone() }
                }
            }
        })
    }

    fun setString(section: String, key: String, value: String): Boolean =
        try {
            engine?.setString(section, key, value) ?: false
        } catch (t: Throwable) {
            false
        }

    fun stop() {
        isStarting = false
        readyAt = null
        engine?.destroy()
        engine = null
        main.post { onEngineState?.invoke() }
    }
}
