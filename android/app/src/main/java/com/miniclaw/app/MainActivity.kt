package com.miniclaw.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.os.Bundle
import android.text.InputType
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast

/**
 * Single-screen chat UX: message list on top, composer at the bottom,
 * engine status in the header. LLM settings live in a dialog and are applied
 * to the running engine via setString().
 */
class MainActivity : Activity() {

    private lateinit var client: EngineClient
    private lateinit var list: ListView
    private lateinit var input: EditText
    private lateinit var statusView: TextView
    private lateinit var adapter: MsgAdapter

    private val messages = ArrayList<Msg>()
    private var streamingIndex = -1 // agent bubble currently receiving tokens

    data class Msg(val who: Int, var text: String) {
        companion object { const val AGENT = 0; const val USER = 1 }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        list = findViewById(R.id.messages)
        input = findViewById(R.id.input)
        statusView = findViewById(R.id.status)
        findViewById<android.widget.Button>(R.id.send).setOnClickListener { onSend() }
        findViewById<TextView>(R.id.settings_btn).setOnClickListener { showSettings() }

        adapter = MsgAdapter(this, messages)
        list.adapter = adapter

        client = EngineClient(filesDir.absolutePath)
        client.setUiListener(object : EngineClient.UiListener {
            override fun onToken(text: String) = appendAgent(text)
            override fun onToolStart(content: String) =
                appendAgent("\n⚙ " + firstLine(content).take(80))
            override fun onToolEnd(content: String) {
                val out = firstLine(content).take(80)
                if (out.isNotEmpty()) appendAgent("  → " + out)
            }
            override fun onStatus(text: String) { statusView.text = text }
            override fun onError(text: String) {
                statusView.text = "error"
                appendAgent("\n⚠ " + firstLine(text))
            }
            override fun onDone() {
                statusView.text = "ready"
                streamingIndex = -1
            }
        })

        applySavedSettings()
    }

    override fun onStart() {
        super.onStart()
        client.start()
    }

    override fun onStop() {
        client.stop()
        super.onStop()
    }

    private fun onSend() {
        val text = input.text.toString().trim()
        if (text.isEmpty()) return
        if (!client.isReady()) {
            Toast.makeText(this, "engine not ready", Toast.LENGTH_SHORT).show()
            return
        }
        input.text.clear()
        messages.add(Msg(Msg.USER, text))
        streamingIndex = -1
        adapter.notifyDataSetChanged()
        scrollBottom()
        client.send("main", text)
    }

    /** Append to the current agent bubble, creating one if needed. */
    private fun appendAgent(chunk: String) {
        if (streamingIndex < 0 || streamingIndex >= messages.size ||
            messages[streamingIndex].who != Msg.AGENT
        ) {
            messages.add(Msg(Msg.AGENT, ""))
            streamingIndex = messages.size - 1
        }
        messages[streamingIndex].text += chunk
        adapter.notifyDataSetChanged()
        scrollBottom()
    }

    private fun scrollBottom() {
        if (messages.isNotEmpty()) list.setSelection(messages.size - 1)
    }

    // ── settings ────────────────────────────────────────────────────────────

    private fun prefs() = getSharedPreferences("miniclaw", Context.MODE_PRIVATE)

    private fun applySavedSettings() {
        val p = prefs()
        val endpoint = p.getString("endpoint", "") ?: ""
        val model = p.getString("model", "") ?: ""
        val key = p.getString("api_key", "") ?: ""
        if (endpoint.isNotEmpty()) client.setString("conversation", "endpoint", endpoint)
        if (model.isNotEmpty()) client.setString("conversation", "model", model)
        if (key.isNotEmpty()) client.setString("conversation", "api_key", key)
    }

    private fun showSettings() {
        val p = prefs()
        val fEndpoint = EditText(this).apply { hint = "https://host/v1/chat/completions" }
        fEndpoint.setText(p.getString("endpoint", ""))
        val fModel = EditText(this).apply { hint = "gpt-4o-mini" }
        fModel.setText(p.getString("model", ""))
        val fKey = EditText(this).apply {
            hint = "api key"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        fKey.setText(p.getString("api_key", ""))

        val pad = (16 * resources.displayMetrics.density).toInt()
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad / 2, pad, 0)
            addView(fEndpoint)
            addView(fModel)
            addView(fKey)
        }

        AlertDialog.Builder(this)
            .setTitle("LLM settings (conversation)")
            .setView(container)
            .setPositiveButton("Save") { _, _ ->
                p.edit()
                    .putString("endpoint", fEndpoint.text.toString())
                    .putString("model", fModel.text.toString())
                    .putString("api_key", fKey.text.toString())
                    .apply()
                client.setString("conversation", "endpoint", fEndpoint.text.toString())
                client.setString("conversation", "model", fModel.text.toString())
                client.setString("conversation", "api_key", fKey.text.toString())
                Toast.makeText(this, "saved", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun firstLine(s: String): String =
        s.lineSequence().firstOrNull()?.trim() ?: ""

    // ── message list adapter ────────────────────────────────────────────────

    private inner class MsgAdapter(ctx: Context, val items: List<Msg>) :
        ArrayAdapter<Msg>(ctx, 0, items) {
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: LayoutInflater.from(context)
                .inflate(R.layout.item_message, parent, false)
            val msg = items[position]
            val bubble = v.findViewById<TextView>(R.id.bubble)
            val who = v.findViewById<TextView>(R.id.who)
            val row = v.findViewById<LinearLayout>(R.id.row)
            bubble.text = msg.text
            if (msg.who == Msg.USER) {
                who.text = "you"
                bubble.setBackgroundResource(R.drawable.bubble_user)
                row.gravity = android.view.Gravity.END
            } else {
                who.text = "miniclaw"
                bubble.setBackgroundResource(R.drawable.bubble_agent)
                row.gravity = android.view.Gravity.START
            }
            return v
        }
    }
}
