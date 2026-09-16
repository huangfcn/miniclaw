package com.miniclaw.app

import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * On-device MNN models: catalog, downloads into <workspace>/models/, and
 * engine load status. Logic mirror of ios/Miniclaw/State/ModelStore.swift +
 * Engine/Models.swift; the UI lives in ModelsTab.kt.
 *
 * Downloads are plain HttpURLConnection GETs (no resume — a failed file
 * restarts). Progress is posted to the main thread at most ~4x/sec.
 */
class ModelManager(private val client: EngineClient) {

    enum class Kind { LLM, EMBEDDING }

    data class LocalModel(
        val id: String,
        val kind: Kind,
        val name: String,
        val blurb: String,
        val dirName: String,
        val files: List<String>,
        /** HuggingFace resolve base URL; null = one-time conversion required. */
        val repoUrl: String?,
        val sizeBytes: Long,
    )

    data class SlotStatus(val loaded: Boolean, val dir: String, val message: String)

    sealed class DownloadState {
        object Idle : DownloadState()
        data class Downloading(val progress: Float, val detail: String) : DownloadState()
        object Done : DownloadState()
        data class Failed(val message: String) : DownloadState()
    }

    /** Fired on the main thread whenever any state changes; UI re-renders. */
    var onState: (() -> Unit)? = null

    companion object {
        val CATALOG = listOf(
            LocalModel(
                id = "qwen3-1.7b",
                kind = Kind.LLM,
                name = "Qwen3 1.7B (int4)",
                blurb = "Chat & summarization — runs the agent loop and memory distillation on-device.",
                dirName = "qwen3-1.7b",
                files = listOf("llm_config.json", "tokenizer.txt", "llm.mnn", "llm.mnn.weight"),
                repoUrl = "https://huggingface.co/taobao-mnn/Qwen3-1.7B-MNN/resolve/main/",
                sizeBytes = 1_240_000_000L,
            ),
            LocalModel(
                id = "qwen3.5-2b",
                kind = Kind.LLM,
                name = "Qwen3.5 2B (int4)",
                blurb = "Stronger chat & talk summarization — vision-language model, all files required.",
                dirName = "qwen3.5-2b",
                // Qwen3.5-MNN is a VLM (is_visual: true): the engine loads the
                // vision tower unconditionally, so visual.mnn + visual.mnn.weight are
                // mandatory or the load fails.
                files = listOf(
                    "llm_config.json", "tokenizer.txt", "llm.mnn",
                    "visual.mnn", "visual.mnn.weight", "llm.mnn.weight",
                ),
                repoUrl = "https://huggingface.co/taobao-mnn/Qwen3.5-2B-MNN/resolve/main/",
                sizeBytes = 1_400_000_000L,
            ),
            LocalModel(
                id = "qwen3.5-4b",
                kind = Kind.LLM,
                name = "Qwen3.5 4B (int4)",
                blurb = "Best local quality — for high-end devices with RAM to spare (~3GB download, slower).",
                dirName = "qwen3.5-4b",
                // Same VLM layout as the 2B: visual.mnn + visual.mnn.weight are
                // mandatory or the load fails.
                files = listOf(
                    "llm_config.json", "tokenizer.txt", "llm.mnn",
                    "visual.mnn", "visual.mnn.weight", "llm.mnn.weight",
                ),
                repoUrl = "https://huggingface.co/taobao-mnn/Qwen3.5-4B-MNN/resolve/main/",
                sizeBytes = 3_000_000_000L,
            ),
            LocalModel(
                id = "bge-m3",
                kind = Kind.EMBEDDING,
                name = "BGE-M3",
                blurb = "Multilingual embeddings for the memory index. Requires a one-time conversion (docs/LOCAL_MODELS.md).",
                dirName = "bge-m3",
                // embeddings_bf16.bin is the word-embedding table the engine
                // looks up before feeding the graph (required, ~512 MB).
                files = listOf("llm_config.json", "tokenizer.txt", "embeddings_bf16.bin", "llm.mnn", "llm.mnn.weight"),
                repoUrl = null,
                sizeBytes = 2_200_000_000L,
            ),
        )
    }

    private val main = Handler(Looper.getMainLooper())
    private val downloads = HashMap<String, DownloadState>()
    private var llmSlot = SlotStatus(false, "", "")
    private var embSlot = SlotStatus(false, "", "")
    private val activeConns = HashMap<String, HttpURLConnection>()

    fun modelsDir(): File = File(client.workspaceDir, "models")

    fun stateOf(modelId: String): DownloadState = downloads[modelId] ?: DownloadState.Idle

    fun slot(kind: Kind): SlotStatus = if (kind == Kind.LLM) llmSlot else embSlot

    fun isDownloaded(model: LocalModel): Boolean {
        val dir = File(modelsDir(), model.dirName)
        return model.files.all { File(dir, it).length() > 0 }
    }

    /** Parse mc_local_status() JSON into slot states. Main thread. */
    fun refreshStatus() {
        val json = try {
            client.engine?.localStatus()
        } catch (t: Throwable) {
            null
        } ?: return
        try {
            val root = JSONObject(json)
            llmSlot = parseSlot(root, "llm")
            embSlot = parseSlot(root, "embedding")
        } catch (t: Throwable) {
            // malformed — keep previous state
        }
    }

    private fun parseSlot(root: JSONObject, key: String): SlotStatus {
        val o = root.optJSONObject(key) ?: return SlotStatus(false, "", "")
        return SlotStatus(o.optBoolean("loaded"), o.optString("dir"), o.optString("message"))
    }

    /** Download all missing files sequentially on a worker thread. */
    fun download(model: LocalModel, customUrl: String? = null) {
        val base = when {
            !customUrl.isNullOrBlank() -> customUrl.trim().let { if (it.endsWith("/")) it else "$it/" }
            model.repoUrl != null -> model.repoUrl
            else -> {
                setState(model.id, DownloadState.Failed("No download source — convert BGE-M3 first (docs/LOCAL_MODELS.md)."))
                return
            }
        }
        if (activeConns.containsKey(model.id)) return  // already downloading

        val dir = File(modelsDir(), model.dirName).apply { mkdirs() }
        val total = model.sizeBytes.coerceAtLeast(1L)

        val thread = Thread {
            var doneBefore = 0L
            for (name in model.files) {
                val dest = File(dir, name)
                if (dest.length() > 0) {
                    doneBefore += dest.length()
                    continue
                }
                setState(model.id, DownloadState.Downloading(doneBefore / total.toFloat(), "Downloading $name…"))
                try {
                    val conn = URL(base + name).openConnection() as HttpURLConnection
                    conn.connectTimeout = 30_000
                    conn.readTimeout = 60_000
                    activeConns[model.id] = conn
                    val tmp = File(dir, "$name.part")
                    conn.inputStream.use { input ->
                        tmp.outputStream().use { out ->
                            val buf = ByteArray(1 shl 16)
                            var lastPost = 0L
                            while (true) {
                                val n = input.read(buf)
                                if (n < 0) break
                                out.write(buf, 0, n)
                                doneBefore += n
                                val now = System.currentTimeMillis()
                                if (now - lastPost > 250) {
                                    lastPost = now
                                    val p = doneBefore / total.toFloat()
                                    setState(model.id, DownloadState.Downloading(p.coerceIn(0f, 1f), "Downloading $name…"))
                                }
                            }
                        }
                    }
                    conn.disconnect()
                    tmp.renameTo(dest)
                } catch (t: Throwable) {
                    setState(model.id, DownloadState.Failed("$name: ${t.message ?: t.javaClass.simpleName}"))
                    return@Thread
                } finally {
                    activeConns.remove(model.id)
                }
            }
            setState(model.id, DownloadState.Done)
        }
        thread.start()
    }

    /** Abort an in-flight download by closing its connection. */
    fun cancel(modelId: String) {
        try {
            activeConns[modelId]?.disconnect()
        } catch (t: Throwable) {
            // ignore
        }
        activeConns.remove(modelId)
        setState(modelId, DownloadState.Failed("Cancelled."))
    }

    /** Ask the engine to load the model in the background; poll status. */
    fun loadInEngine(model: LocalModel) {
        val kind = if (model.kind == Kind.LLM) "llm" else "embedding"
        try {
            client.engine?.localLoad(kind)
        } catch (t: Throwable) {
            // engine not ready — status refresh will show the message
        }
        for (delay in longArrayOf(2_000, 5_000, 10_000, 20_000, 40_000)) {
            main.postDelayed({
                refreshStatus()
                onState?.invoke()
            }, delay)
        }
    }

    private fun setState(modelId: String, state: DownloadState) {
        main.post {
            downloads[modelId] = state
            onState?.invoke()
        }
    }
}
