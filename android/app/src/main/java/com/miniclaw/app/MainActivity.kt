package com.miniclaw.app

import android.app.Activity
import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.ListView
import android.widget.TextView

/**
 * Three-tab UX (Chat / Status / Settings) — native port of the React frontend's
 * mobile views and the iOS SwiftUI app. The engine runs in-process via JNI:
 * MainActivity → EngineClient → NativeEngine → libminiclaw_core.so.
 */
class MainActivity : Activity() {

    // ── message model (mirrors ios ChatMessage) ─────────────────────────────

    data class Msg(
        val who: Int,
        var text: String = "",
        val activities: ArrayList<String> = ArrayList(),
        var isError: Boolean = false,
    ) {
        companion object { const val AGENT = 0; const val USER = 1 }
    }

    private lateinit var client: EngineClient
    private val messages = ArrayList<Msg>()
    private var streamingIndex = -1   // agent message currently receiving events
    private var isSending = false

    // chat views
    private lateinit var list: ListView
    private lateinit var input: EditText
    private lateinit var sendBtn: TextView
    private lateinit var emptyState: View
    private lateinit var emptySubtitle: TextView
    private lateinit var thinking: View
    private lateinit var adapter: MsgAdapter

    // status views
    private lateinit var statusCard: View
    private lateinit var statusIcon: TextView
    private lateinit var statusTitle: TextView
    private lateinit var errorCard: View
    private lateinit var errorMsg: TextView
    private lateinit var rowVersion: TextView
    private lateinit var rowStatus: TextView
    private lateinit var rowUptime: TextView
    private lateinit var rowModel: TextView
    private lateinit var rowEndpoint: TextView
    private lateinit var rowWorkspace: TextView

    // settings views
    private lateinit var setEndpoint: EditText
    private lateinit var setModel: EditText
    private lateinit var setApiKey: EditText
    private lateinit var setBraveKey: EditText
    private lateinit var saveLlm: TextView
    private lateinit var yamlEditor: EditText
    private lateinit var yamlSave: TextView
    private lateinit var yamlNotice: TextView
    private lateinit var infoProvider: TextView

    // tab views + nav labels
    private lateinit var tabChat: View
    private lateinit var tabStatus: View
    private lateinit var tabSettings: View
    private val navLabels = intArrayOf(R.id.tab_chat_label, R.id.tab_status_label, R.id.tab_settings_label)
    private var activeTab = 0

    /** Snapshot of the yaml at last load; dirty = editor text differs. */
    private var yamlLoaded: String? = null
    private var saveNotice: String? = null

    private val handler = Handler(Looper.getMainLooper())
    private val uptimeTick = object : Runnable {
        override fun run() {
            updateUptime()
            handler.postDelayed(this, 1000)
        }
    }

    // ── lifecycle ───────────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bindViews()
        wireChat()
        wireSettings()
        wireTabs()

        client = EngineClient(filesDir.absolutePath)
        client.setUiListener(object : EngineClient.UiListener {
            override fun onToken(text: String) = appendAgent(text)
            override fun onToolStart(content: String) =
                appendActivity("🔧 " + firstLine(content).take(80))
            override fun onToolEnd(content: String) {
                val out = firstLine(content).take(80)
                if (out.isNotEmpty()) appendActivity("✓  " + out)
            }
            override fun onStatus(text: String) = appendActivity("⚡ " + firstLine(text))
            override fun onError(text: String) {
                // "error" is terminal (the turn ends with done OR error), so
                // clear the sending state here too — mirrors ios AppState.apply.
                val m = currentAgent()
                if (m != null) {
                    m.isError = true
                    if (m.text.isEmpty()) m.text = text
                    else if (text.isNotEmpty()) m.activities.add("⚠ " + firstLine(text))
                }
                isSending = false
                streamingIndex = -1
                thinking.visibility = View.GONE
                sendEnabled()
                refreshList()
            }
            override fun onDone() {
                isSending = false
                streamingIndex = -1
                thinking.visibility = View.GONE
                sendEnabled()
                refreshList()
            }
        })
        client.onEngineState = { syncEngineUi() }

        applySavedSettings()
    }

    override fun onStart() {
        super.onStart()
        client.start()
        syncEngineUi()
    }

    override fun onStop() {
        handler.removeCallbacks(uptimeTick)
        client.stop()
        super.onStop()
    }

    // ── view binding ────────────────────────────────────────────────────────

    private fun bindViews() {
        tabChat = findViewById(R.id.tab_chat_root)
        tabStatus = findViewById(R.id.tab_status_root)
        tabSettings = findViewById(R.id.tab_settings_root)

        list = findViewById(R.id.messages)
        input = findViewById(R.id.input)
        sendBtn = findViewById(R.id.send)
        emptyState = findViewById(R.id.empty_state)
        emptySubtitle = findViewById(R.id.empty_subtitle)
        thinking = findViewById(R.id.thinking)
        adapter = MsgAdapter(this, messages)
        list.adapter = adapter

        statusCard = findViewById(R.id.status_card)
        statusIcon = findViewById(R.id.status_icon)
        statusTitle = findViewById(R.id.status_title)
        errorCard = findViewById(R.id.error_card)
        errorMsg = findViewById(R.id.error_msg)
        val rows = listOf(
            R.id.row_version to "VERSION",
            R.id.row_status to "STATUS",
            R.id.row_uptime to "UPTIME",
            R.id.row_model to "MODEL",
            R.id.row_endpoint to "ENDPOINT",
            R.id.row_workspace to "WORKSPACE",
        )
        val values = ArrayList<TextView>(rows.size)
        for ((rowId, label) in rows) {
            val row = findViewById<View>(rowId)
            row.findViewById<TextView>(R.id.label).text = label
            values.add(row.findViewById(R.id.value))
        }
        rowVersion = values[0]; rowStatus = values[1]; rowUptime = values[2]
        rowModel = values[3]; rowEndpoint = values[4]; rowWorkspace = values[5]

        setEndpoint = findViewById(R.id.set_endpoint)
        setModel = findViewById(R.id.set_model)
        setApiKey = findViewById(R.id.set_api_key)
        setBraveKey = findViewById(R.id.set_brave_key)
        saveLlm = findViewById(R.id.save_llm)
        yamlEditor = findViewById(R.id.yaml_editor)
        yamlSave = findViewById(R.id.yaml_save)
        yamlNotice = findViewById(R.id.yaml_notice)
        infoProvider = findViewById(R.id.info_provider)

        findViewById<View>(R.id.tab_chat).setOnClickListener { selectTab(0) }
        findViewById<View>(R.id.tab_status).setOnClickListener { selectTab(1) }
        findViewById<View>(R.id.tab_settings).setOnClickListener { selectTab(2) }
    }

    private fun wireChat() {
        sendBtn.setOnClickListener { onSend() }
        input.setOnEditorActionListener { v, actionId, _ ->
            if (actionId == android.view.inputmethod.EditorInfo.IME_ACTION_SEND) {
                onSend(); true
            } else false
        }
        input.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable?) = sendEnabled()
        })
    }

    private fun wireSettings() {
        saveLlm.setOnClickListener { saveStructuredSettings() }
        findViewById<View>(R.id.yaml_reload).setOnClickListener { loadYaml() }
        yamlSave.setOnClickListener { saveYaml() }
        yamlEditor.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable?) = yamlDirtyUpdate()
        })
    }

    private fun wireTabs() {
        selectTab(0)
    }

    private fun selectTab(i: Int) {
        activeTab = i
        tabChat.visibility = if (i == 0) View.VISIBLE else View.GONE
        tabStatus.visibility = if (i == 1) View.VISIBLE else View.GONE
        tabSettings.visibility = if (i == 2) View.VISIBLE else View.GONE
        val accent = resources.getColor(R.color.accent, null)
        val tertiary = resources.getColor(R.color.text_tertiary, null)
        for (j in navLabels.indices) {
            findViewById<TextView>(navLabels[j]).setTextColor(if (j == i) accent else tertiary)
        }
        if (i == 1) {
            syncEngineUi()
            handler.post(uptimeTick)
        } else {
            handler.removeCallbacks(uptimeTick)
        }
        if (i == 2 && yamlLoaded == null) loadYaml()
    }

    // ── engine state → UI (chat hero, status tab, composer) ────────────────

    private fun syncEngineUi() {
        val err = client.startError
        val ready = client.isReady()

        // chat empty-state subtitle
        emptySubtitle.text = when {
            err != null -> "Engine error — see Status tab"
            ready -> "Your always-on assistant. What should we work on?"
            else -> "Starting up…"
        }

        // status card
        when {
            err != null -> {
                statusCard.setBackgroundResource(R.drawable.bg_status_error)
                statusIcon.text = "✕"; statusIcon.setTextColor(color(R.color.error))
                statusTitle.text = "Engine failed to start"; statusTitle.setTextColor(color(R.color.error))
                errorCard.visibility = View.VISIBLE
                errorMsg.text = err
            }
            ready -> {
                statusCard.setBackgroundResource(R.drawable.bg_status_ok)
                statusIcon.text = "✓"; statusIcon.setTextColor(color(R.color.ok))
                statusTitle.text = "Engine running"; statusTitle.setTextColor(color(R.color.ok))
                errorCard.visibility = View.GONE
            }
            else -> {
                statusCard.setBackgroundResource(R.drawable.bg_status_warn)
                statusIcon.text = "⚙"; statusIcon.setTextColor(color(R.color.warn))
                statusTitle.text = "Engine starting…"; statusTitle.setTextColor(color(R.color.warn))
                errorCard.visibility = View.GONE
            }
        }

        // info rows
        rowVersion.text = client.version()
        rowStatus.text = if (err != null) "error" else if (ready) "running" else "starting"
        rowModel.text = setModel.text.toString().ifEmpty { "default" }
        rowEndpoint.text = setEndpoint.text.toString().ifEmpty { "default" }
        rowWorkspace.text = client.workspaceDir
        updateUptime()

        infoProvider.text = setModel.text.toString().ifEmpty { "default" }
        sendEnabled()
    }

    private fun updateUptime() {
        val start = client.readyAt
        if (start == null) {
            rowUptime.text = "—"
            return
        }
        val s = maxOf(0, (System.currentTimeMillis() - start) / 1000).toInt()
        rowUptime.text = String.format("%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60)
    }

    private fun color(id: Int): Int = resources.getColor(id, null)

    // ── chat ────────────────────────────────────────────────────────────────

    private fun onSend() {
        val text = input.text.toString().trim()
        if (text.isEmpty() || isSending || !client.isReady()) return
        input.text.clear()
        messages.add(Msg(Msg.USER, text))
        messages.add(Msg(Msg.AGENT))   // bubble exists before the first event (ios parity)
        streamingIndex = messages.size - 1
        isSending = true
        thinking.visibility = View.VISIBLE
        refreshList()
        client.send("main", text)
    }

    /** Append a token chunk to the current agent message, creating one if needed. */
    private fun appendAgent(chunk: String) {
        val m = currentAgent() ?: return
        m.text += chunk
        refreshList()
    }

    private fun appendActivity(line: String) {
        val m = currentAgent() ?: return
        m.activities.add(line)
        refreshList()
    }

    private fun currentAgent(): Msg? {
        if (streamingIndex in messages.indices && messages[streamingIndex].who == Msg.AGENT) {
            return messages[streamingIndex]
        }
        messages.add(Msg(Msg.AGENT))
        streamingIndex = messages.size - 1
        return messages[streamingIndex]
    }

    private fun refreshList() {
        val empty = messages.isEmpty()
        emptyState.visibility = if (empty) View.VISIBLE else View.GONE
        list.visibility = if (empty) View.GONE else View.VISIBLE
        adapter.notifyDataSetChanged()
        if (!empty) list.setSelection(messages.size - 1)
    }

    private fun sendEnabled() {
        val ok = client.isReady() && !isSending &&
            input.text.toString().trim().isNotEmpty()
        sendBtn.isEnabled = ok
        sendBtn.setTextColor(color(if (ok) R.color.white else R.color.text_tertiary))
    }

    // ── settings: structured LLM form (live-applied, persisted) ────────────

    private fun prefs() = getSharedPreferences("miniclaw", Context.MODE_PRIVATE)

    private fun applySavedSettings() {
        val p = prefs()
        setEndpoint.setText(p.getString("endpoint", ""))
        setModel.setText(p.getString("model", ""))
        setApiKey.setText(p.getString("api_key", ""))
        setBraveKey.setText(p.getString("brave_api_key", ""))
    }

    private fun saveStructuredSettings() {
        val endpoint = setEndpoint.text.toString().trim()
        val model = setModel.text.toString().trim()
        val key = setApiKey.text.toString().trim()
        val braveKey = setBraveKey.text.toString().trim()

        prefs().edit()
            .putString("endpoint", endpoint)
            .putString("model", model)
            .putString("api_key", key)
            .putString("brave_api_key", braveKey)
            .apply()

        var ok = true
        if (client.isReady()) {
            // Live-apply to the running engine; empty values are left untouched.
            if (endpoint.isNotEmpty()) ok = client.setString("conversation", "endpoint", endpoint) && ok
            if (model.isNotEmpty()) ok = client.setString("conversation", "model", model) && ok
            if (key.isNotEmpty()) ok = client.setString("conversation", "api_key", key) && ok
            if (braveKey.isNotEmpty()) ok = client.setString("web", "brave_api_key", braveKey) && ok
        }
        saveNotice = if (ok) "saved" else "failed"
        saveLlm.text = if (ok) "SAVED ✓" else "SAVE FAILED"
        saveLlm.setTextColor(color(if (ok) R.color.ok else R.color.error))
        handler.postDelayed({
            saveNotice = null
            saveLlm.text = "SAVE SETTINGS"
            saveLlm.setTextColor(color(R.color.white))
        }, 1500)
        syncEngineUi()
    }

    // ── settings: raw config.yaml editor (mc_get_config / mc_set_config) ───

    private fun loadYaml() {
        val yaml = client.getConfig() ?: ""
        yamlEditor.setText(yaml)
        yamlLoaded = yaml
        saveNotice = null
        yamlDirtyUpdate()
    }

    private fun yamlDirty(): Boolean = yamlLoaded != null && yamlEditor.text.toString() != yamlLoaded

    private fun yamlDirtyUpdate() {
        val dirty = yamlDirty()
        yamlSave.setTextColor(color(if (dirty) R.color.warn else R.color.text_tertiary))
    }

    private fun saveYaml() {
        if (!yamlDirty()) return
        val ok = client.setConfig(yamlEditor.text.toString())
        if (ok) {
            yamlLoaded = yamlEditor.text.toString()
            showYamlNotice("Configuration saved.", R.color.ok)
        } else {
            showYamlNotice("Save failed — check yaml (or engine not ready).", R.color.error)
        }
        yamlDirtyUpdate()
    }

    private fun showYamlNotice(text: String, colorId: Int) {
        saveNotice = text
        yamlNotice.visibility = View.VISIBLE
        yamlNotice.text = text
        yamlNotice.setTextColor(color(colorId))
    }

    // ── helpers ─────────────────────────────────────────────────────────────

    private fun firstLine(s: String): String =
        s.lineSequence().firstOrNull()?.trim() ?: ""

    // ── message list adapter ────────────────────────────────────────────────

    private inner class MsgAdapter(ctx: Context, val items: List<Msg>) :
        ArrayAdapter<Msg>(ctx, 0, items) {

        override fun getViewTypeCount(): Int = 2
        override fun getItemViewType(position: Int): Int = items[position].who

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val msg = items[position]
            val agent = msg.who == Msg.AGENT
            val layout = if (agent) R.layout.item_message_agent else R.layout.item_message_user
            val v = if (convertView != null && convertView.tag == layout) convertView
                    else LayoutInflater.from(context).inflate(layout, parent, false).also { it.tag = layout }

            val bubble = v.findViewById<TextView>(R.id.bubble)
            bubble.text = msg.text
            bubble.visibility = if (msg.text.isEmpty()) View.GONE else View.VISIBLE

            if (agent) {
                val acts = v.findViewById<TextView>(R.id.activities)
                if (msg.activities.isNotEmpty()) {
                    acts.visibility = View.VISIBLE
                    acts.text = msg.activities.joinToString("\n")
                } else {
                    acts.visibility = View.GONE
                }
                val avatar = v.findViewById<TextView>(R.id.avatar)
                val avatarBg = v.findViewById<View>(R.id.avatar_bg)
                if (msg.isError) {
                    avatarBg.setBackgroundResource(R.drawable.bg_avatar_agent_error)
                    avatar.text = "⚠"
                    avatar.setTextColor(color(R.color.error))
                } else {
                    avatarBg.setBackgroundResource(R.drawable.bg_avatar_agent)
                    avatar.text = "✦"
                    avatar.setTextColor(color(R.color.accent))
                }
            }
            return v
        }
    }
}
