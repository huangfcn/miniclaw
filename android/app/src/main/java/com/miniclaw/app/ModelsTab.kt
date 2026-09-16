package com.miniclaw.app

import android.content.Context
import android.graphics.Typeface
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.Switch
import android.widget.TextView

/**
 * Programmatic UI for the Models tab — port of ios/Miniclaw/Views/LocalModelsView.swift.
 * Cards are built in code (not XML) because their controls change with
 * download / load state. All rendering happens on the main thread.
 */
class ModelsTab(
    private val ctx: Context,
    private val client: EngineClient,
    private val manager: ModelManager,
    private val cardsContainer: LinearLayout,
    private val dirLabel: TextView,
) {

    private data class CardRefs(
        val status: TextView,
        val controls: LinearLayout,
        val toggleRow: LinearLayout,
        val urlField: EditText?,
    )

    private val cards = HashMap<String, CardRefs>()

    fun bind() {
        dirLabel.text = manager.modelsDir().absolutePath
        for (model in ModelManager.CATALOG) buildCard(model)
        renderAll()
    }

    // ── card construction ───────────────────────────────────────────────────

    private fun buildCard(model: ModelManager.LocalModel) {
        val card = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundResource(R.drawable.bg_card)
            val p = dp(16)
            setPadding(p, p, p, p)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { bottomMargin = dp(12) }
        }

        // title row: name + kind badge
        val titleRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            )
        }
        val name = TextView(ctx).apply {
            text = model.name
            textSize = 15f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(color(R.color.text_primary))
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        val badge = TextView(ctx).apply {
            text = if (model.kind == ModelManager.Kind.LLM) "CHAT" else "EMBEDDING"
            textSize = 9f
            setTypeface(Typeface.MONOSPACE)
            setTextColor(color(R.color.accent))
        }
        titleRow.addView(name)
        titleRow.addView(badge)
        card.addView(titleRow)

        // blurb
        val blurb = TextView(ctx).apply {
            text = model.blurb
            textSize = 12f
            setTextColor(color(R.color.text_secondary))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(6) }
        }
        card.addView(blurb)

        // engine status row
        val status = TextView(ctx).apply {
            textSize = 11f
            setTextColor(color(R.color.text_tertiary))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(8) }
        }
        card.addView(status)

        // controls (rebuilt on every state change)
        val controls = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(10) }
        }
        card.addView(controls)

        // custom URL field (BGE-M3 only — no off-the-shelf MNN build exists)
        var urlField: EditText? = null
        if (model.repoUrl == null) {
            urlField = EditText(ctx).apply {
                hint = "Optional: paste base URL of a converted BGE-M3 build…"
                textSize = 12f
                inputType = InputType.TYPE_TEXT_VARIATION_URI
                setTextColor(color(R.color.text_primary))
                setHintTextColor(color(R.color.text_tertiary))
                setBackgroundResource(R.drawable.bg_field)
                val p = dp(10)
                setPadding(p, p, p, p)
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                ).apply { topMargin = dp(8) }
            }
            card.addView(urlField)
        }

        // provider toggle (shown once the files are on disk)
        val toggleRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            visibility = View.GONE
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(10) }
        }
        val toggleLabel = TextView(ctx).apply {
            text = if (model.kind == ModelManager.Kind.LLM) "Use on-device chat" else "Use on-device embeddings"
            textSize = 12f
            setTextColor(color(R.color.text_secondary))
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        val toggle = Switch(ctx).apply {
            isChecked = prefMnn(model.kind)
            setOnCheckedChangeListener { _, checked ->
                prefs().edit().putString(prefKey(model.kind), if (checked) "mnn" else "openai").apply()
                applyProvider(model.kind, checked)
            }
        }
        toggleRow.addView(toggleLabel)
        toggleRow.addView(toggle)
        card.addView(toggleRow)

        cardsContainer.addView(card)
        cards[model.id] = CardRefs(status, controls, toggleRow, urlField)
    }

    // ── state → UI ──────────────────────────────────────────────────────────

    fun renderAll() {
        for (model in ModelManager.CATALOG) render(model)
    }

    private fun render(model: ModelManager.LocalModel) {
        val refs = cards[model.id] ?: return
        val state = manager.stateOf(model.id)
        val slot = manager.slot(model.kind)
        val downloaded = manager.isDownloaded(model)
        val sizeGb = model.sizeBytes / 1_000_000_000f

        // status row
        refs.status.text = when {
            slot.loaded -> "● loaded in engine"
            state is ModelManager.DownloadState.Downloading -> "↓ ${state.detail}"
            downloaded -> "✓ on disk — not loaded yet"
            else -> "○ not downloaded (~${String.format("%.1f", sizeGb)} GB)"
        }
        refs.status.setTextColor(
            color(if (slot.loaded) R.color.ok
            else if (downloaded) R.color.warn
            else R.color.text_tertiary)
        )

        // toggle visible once files exist
        refs.toggleRow.visibility = if (downloaded) View.VISIBLE else View.GONE

        // controls
        val c = refs.controls
        c.removeAllViews()
        when {
            state is ModelManager.DownloadState.Downloading -> {
                c.addView(progressBar(state))
                c.addView(actionRow(
                    button("CANCEL") { manager.cancel(model.id) },
                    spacer(),
                    TextView(ctx).apply {
                        text = "${(state.progress * 100).toInt()}%"
                        textSize = 11f
                        setTextColor(color(R.color.text_secondary))
                    },
                ))
            }
            downloaded -> {
                if (!slot.loaded) {
                    c.addView(actionRow(button("LOAD IN ENGINE") { manager.loadInEngine(model) }))
                } else {
                    c.addView(statusLine("Ready — active when the provider toggle is on.", R.color.ok))
                }
            }
            state is ModelManager.DownloadState.Failed -> {
                c.addView(statusLine(state.message, R.color.error))
                c.addView(actionRow(button("RETRY") {
                    manager.download(model, refs.urlField?.text?.toString())
                }))
            }
            else -> {
                if (model.repoUrl != null) {
                    c.addView(actionRow(
                        button("DOWNLOAD (~${String.format("%.1f", sizeGb)} GB)") { manager.download(model) },
                    ))
                } else {
                    c.addView(statusLine(
                        "No off-the-shelf MNN build. Convert once on a dev machine (docs/LOCAL_MODELS.md), then paste the base URL above.",
                        R.color.text_tertiary,
                    ))
                    c.addView(actionRow(button("DOWNLOAD FROM URL") {
                        manager.download(model, refs.urlField?.text?.toString())
                    }))
                }
            }
        }
    }

    private fun progressBar(state: ModelManager.DownloadState.Downloading): ProgressBar {
        return ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            progress = (state.progress * 1000).toInt()
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(8),
            )
        }
    }

    private fun button(label: String, onClick: () -> Unit): TextView {
        return TextView(ctx).apply {
            text = label
            textSize = 12f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(color(R.color.white))
            setBackgroundResource(R.drawable.bg_send_btn)
            gravity = Gravity.CENTER
            val p = dp(10)
            setPadding(dp(18), p, dp(18), p)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            )
            setOnClickListener { onClick() }
        }
    }

    private fun spacer(): View {
        return View(ctx, null).apply {
            layoutParams = LinearLayout.LayoutParams(0, 1, 1f)
        }
    }

    private fun actionRow(vararg children: View): LinearLayout {
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            for (v in children) addView(v)
        }
    }

    private fun statusLine(text: String, colorId: Int): TextView {
        return TextView(ctx).apply {
            this.text = text
            textSize = 11f
            setTextColor(color(colorId))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(4) }
        }
    }

    // ── provider persistence (mirrors ios AppState.applyLocalProvider) ─────

    private fun prefKey(kind: ModelManager.Kind): String = "local_provider_${kind.name.lowercase()}"

    private fun prefMnn(kind: ModelManager.Kind): Boolean =
        prefs().getString(prefKey(kind), "openai") == "mnn"

    private fun applyProvider(kind: ModelManager.Kind, mnn: Boolean) {
        val section = if (kind == ModelManager.Kind.LLM) "conversation" else "embedding"
        try {
            client.setString(section, "provider", if (mnn) "mnn" else "openai")
        } catch (t: Throwable) {
            // engine not ready — re-applied on next start
        }
    }

    private fun prefs() = ctx.getSharedPreferences("miniclaw", Context.MODE_PRIVATE)

    private fun color(id: Int): Int = ctx.resources.getColor(id, null)

    private fun dp(v: Int): Int = (v * ctx.resources.displayMetrics.density).toInt()
}
