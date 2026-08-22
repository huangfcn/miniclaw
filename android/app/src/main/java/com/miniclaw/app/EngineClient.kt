package com.miniclaw.app

import android.os.Handler
import android.os.Looper
import com.miniclaw.core.NativeEngine

/** Owns the native engine; marshals worker-thread events onto the main looper. */
class EngineClient(private val workspaceDir: String) {

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
    @Volatile var startError: String? = null
        private set

    private val main = Handler(Looper.getMainLooper())
    private var listener: UiListener? = null

    fun setUiListener(l: UiListener) {
        listener = l
    }

    /** Start the engine on a background thread (bootstraps the workspace). */
    fun start() {
        Thread {
            try {
                val e = NativeEngine.create(workspaceDir, null)
                engine = e
                main.post { listener?.onStatus("ready") }
            } catch (t: Throwable) {
                startError = t.message ?: "engine start failed"
                main.post { listener?.onError(startError!!) }
            }
        }.start()
    }

    fun isReady(): Boolean = engine?.isRunning() == true

    fun send(sessionId: String, message: String) {
        val e = engine ?: return
        main.post { listener?.onStatus("thinking") }
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
        engine?.setString(section, key, value) ?: false

    fun stop() {
        engine?.destroy()
        engine = null
    }
}
