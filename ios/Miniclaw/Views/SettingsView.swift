import SwiftUI

/// Settings tab — richer than Android's dialog:
///   1. Structured LLM form (endpoint/model/api_key/brave key), applied live
///      via mc_set_string — same fields as the Android settings dialog.
///   2. Advanced raw config.yaml editor (port of frontend Settings.tsx) via
///      mc_get_config / mc_set_config.
struct SettingsView: View {
    @EnvironmentObject private var state: AppState

    private var yamlDirty: Bool { state.yamlText != state.yamlLoaded }

    var body: some View {
        ZStack {
            Theme.page.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    header
                    llmSection
                    yamlSection
                    infoCards
                }
                .padding(.horizontal, 16)
                .padding(.top, 24)
                .padding(.bottom, 32)
            }
        }
        .onAppear { state.loadYaml() }
    }

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: "gearshape.fill")
                .font(.system(size: 18))
                .foregroundStyle(Theme.accent)
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(Theme.accent.opacity(0.1)))
            VStack(alignment: .leading, spacing: 2) {
                Text("Configuration")
                    .font(.system(size: 22, weight: .bold))
                    .foregroundStyle(Theme.textPrimary)
                Text("Refine your engine parameters and model endpoints.")
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(Theme.textSecondary)
            }
        }
    }

    // ── structured LLM form (Android dialog parity) ────────────────────────

    private var llmSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("LLM SETTINGS")
            field("Endpoint", $state.endpoint, placeholder: "https://host/v1/chat/completions")
            field("Model", $state.model, placeholder: "gpt-4o-mini")
            secureField("API Key", $state.apiKey, placeholder: "api key")
            secureField("Brave Search Key (optional)", $state.braveKey, placeholder: "for web_search")

            Button(action: { state.saveStructuredSettings() }) {
                HStack {
                    Image(systemName: "checkmark")
                    Text(state.saveNotice == "saved" ? "Saved!" : "Save Settings")
                }
                .font(.system(size: 14, weight: .bold))
                .foregroundStyle(.white)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
                .background(RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(state.saveNotice == "saved" ? Theme.ok : Theme.accentDim))
            }
            .disabled(!state.isReady)
            .opacity(state.isReady ? 1 : 0.5)
        }
        .card()
    }

    private func field(_ label: String, _ text: Binding<String>,
                       placeholder: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label.uppercased())
                .font(.system(size: 10, weight: .bold))
                .kerning(1.5)
                .foregroundStyle(Theme.textTertiary)
            TextField(placeholder, text: text)
                .font(.system(size: 14, design: .monospaced))
                .foregroundStyle(Theme.textPrimary)
                .tint(Theme.accent)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .padding(12)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(Theme.cardAlt)
                    .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .strokeBorder(Theme.hairline, lineWidth: 1)))
        }
    }

    private func secureField(_ label: String, _ text: Binding<String>,
                             placeholder: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label.uppercased())
                .font(.system(size: 10, weight: .bold))
                .kerning(1.5)
                .foregroundStyle(Theme.textTertiary)
            SecureField(placeholder, text: text)
                .font(.system(size: 14, design: .monospaced))
                .foregroundStyle(Theme.textPrimary)
                .tint(Theme.accent)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .padding(12)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(Theme.cardAlt)
                    .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .strokeBorder(Theme.hairline, lineWidth: 1)))
        }
    }

    // ── advanced raw yaml (frontend Settings.tsx port) ─────────────────────

    private var yamlSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                sectionTitle("ADVANCED — config.yaml")
                Spacer()
                Button("Reload") { state.loadYaml() }
                    .font(.system(size: 12, weight: .bold))
                    .buttonStyle(.plain)
                    .foregroundStyle(Theme.accent)
                Button("Save") { state.saveYaml() }
                    .font(.system(size: 12, weight: .bold))
                    .buttonStyle(.plain)
                    .foregroundStyle(yamlDirty ? Theme.warn : Theme.textTertiary)
                    .disabled(!yamlDirty)
            }

            TextEditor(text: $state.yamlText)
                .font(.system(size: 13, design: .monospaced))
                .foregroundStyle(Theme.accent.opacity(0.9))
                .tint(Theme.accent)
                .scrollContentBackground(.hidden)
                .frame(minHeight: 260)
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(Theme.cardAlt))
                .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(yamlDirty ? Theme.warn.opacity(0.5) : Theme.hairline,
                                  lineWidth: 1))

            if let notice = state.saveNotice {
                Text(notice == "saved" ? "Configuration saved." : "Save failed — check yaml.")
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(notice == "saved" ? Theme.ok : Theme.error)
            }
        }
        .card()
    }

    // ── info cards (frontend Settings.tsx parity) ───────────────────────────

    private var infoCards: some View {
        VStack(spacing: 12) {
            infoCard("Engine Mode", "in-process",
                     "The engine runs inside this app — no server process, no port. It starts on launch and stops when backgrounded.")
            infoCard("LLM Provider", state.model.isEmpty ? "default" : state.model,
                     "Conversation quality uses your configured OpenAI-compatible endpoint. Background tasks can use local models via config.yaml.")
        }
    }

    private func infoCard(_ title: String, _ value: String, _ body: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title.uppercased())
                    .font(.system(size: 10, weight: .bold))
                    .kerning(1.5)
                    .foregroundStyle(Theme.textTertiary)
                Spacer()
                Text(value)
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundStyle(Theme.accent)
            }
            Text(body)
                .font(.system(size: 12))
                .foregroundStyle(Theme.textSecondary)
                .lineSpacing(3)
        }
        .card()
    }

    private func sectionTitle(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 11, weight: .bold))
            .kerning(1.5)
            .foregroundStyle(Theme.textTertiary)
    }
}

private extension View {
    func card() -> some View {
        padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(Theme.card)
                .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .strokeBorder(Theme.hairline, lineWidth: 1)))
    }
}
