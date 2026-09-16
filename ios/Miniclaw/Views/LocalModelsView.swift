import SwiftUI

/// Models tab — on-device MNN inference: download Qwen3 (chat/summarization)
/// and BGE-M3 (embeddings), load them into the engine, and route providers.
struct LocalModelsView: View {
    @EnvironmentObject private var state: AppState
    @ObservedObject private var store: ModelStore

    /// Optional self-hosted URL for a converted BGE-M3 build.
    @State private var bgeURL = ""

    init(store: ModelStore) {
        self.store = store
    }

    var body: some View {
        ZStack {
            Theme.page.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text("Local Models")
                        .font(.system(size: 22, weight: .bold))
                        .foregroundStyle(Theme.textPrimary)
                        .padding(.top, 24)

                    Text("Runs on-device via MNN — no network needed once downloaded. Files live in the workspace models/ folder; conversation quality can stay on your remote API.")
                        .font(.system(size: 12))
                        .foregroundStyle(Theme.textSecondary)
                        .lineSpacing(3)

                    modelCard(LocalModel.catalog[0])
                    modelCard(LocalModel.catalog[1])

                    Text("Models: \(store.modelsDir().path)")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(Theme.textTertiary)
                        .lineLimit(2)
                        .truncationMode(.middle)
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 32)
            }
        }
    }

    // ── one card per catalog model ──────────────────────────────────────────

    private func modelCard(_ model: LocalModel) -> some View {
        let dl = store.downloads[model.id] ?? ModelStore.DownloadState()
        let slot = model.kind == .llm ? store.engineStatus.llm : store.engineStatus.embedding
        let downloaded = store.isDownloaded(model)

        return VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Text(model.name)
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(Theme.textPrimary)
                kindBadge(model.kind)
                Spacer()
                Text(bytesString(model.sizeBytes))
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textTertiary)
            }

            Text(model.blurb)
                .font(.system(size: 12))
                .foregroundStyle(Theme.textSecondary)
                .lineSpacing(3)

            // Engine load status
            HStack(spacing: 8) {
                Circle()
                    .fill(slot.loaded ? Theme.ok : Theme.textTertiary)
                    .frame(width: 8, height: 8)
                Text(slot.loaded
                     ? "Loaded in engine"
                     : (slot.message.isEmpty ? "Not loaded" : slot.message))
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(slot.loaded ? Theme.ok : Theme.textTertiary)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }

            // Download / load controls
            switch dl.phase {
            case .idle:
                if downloaded {
                    HStack(spacing: 10) {
                        Button {
                            store.loadInEngine(model)
                        } label: {
                            Label("Load in engine", systemImage: "arrow.down.circle.fill")
                                .font(.system(size: 13, weight: .semibold))
                        }
                        .buttonStyle(.borderedProminent)
                        Spacer()
                        providerToggle(model)
                    }
                } else if model.repoURL == nil {
                    bgeSection(model, downloaded: downloaded)
                } else {
                    HStack(spacing: 10) {
                        Button {
                            store.download(model)
                        } label: {
                            Label("Download", systemImage: "arrow.down.circle.fill")
                                .font(.system(size: 13, weight: .semibold))
                        }
                        .buttonStyle(.borderedProminent)
                        Spacer()
                    }
                }
            case .downloading:
                VStack(spacing: 8) {
                    ProgressView(value: dl.progress)
                        .tint(Theme.accent)
                    HStack {
                        Text(dl.detail)
                            .font(.system(size: 11))
                            .foregroundStyle(Theme.textTertiary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Spacer()
                        Button("Cancel") { store.cancel(model.id) }
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(Theme.error)
                    }
                }
            case .done:
                HStack(spacing: 10) {
                    Label("Download complete", systemImage: "checkmark.circle.fill")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(Theme.ok)
                    Spacer()
                    if !slot.loaded {
                        Button("Load in engine") { store.loadInEngine(model) }
                            .font(.system(size: 13, weight: .semibold))
                            .buttonStyle(.borderedProminent)
                    }
                    providerToggle(model)
                }
            case .failed(let message):
                VStack(alignment: .leading, spacing: 8) {
                    Label(message, systemImage: "exclamationmark.triangle.fill")
                        .font(.system(size: 12))
                        .foregroundStyle(Theme.error)
                        .lineSpacing(3)
                    HStack(spacing: 10) {
                        Button("Retry") { store.download(model, customURL: model.id == "bge-m3" ? bgeURL : nil) }
                            .font(.system(size: 12, weight: .semibold))
                            .buttonStyle(.bordered)
                        Spacer()
                    }
                }
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(Theme.card)
                .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .strokeBorder(Theme.hairline, lineWidth: 1))
        )
    }

    // ── BGE-M3: no off-the-shelf MNN build — conversion or custom URL ───────

    private func bgeSection(_ model: LocalModel, downloaded: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("No off-the-shelf MNN build exists. Convert once on a dev machine (docs/LOCAL_MODELS.md), host the folder anywhere, then paste its base URL below.")
                .font(.system(size: 11))
                .foregroundStyle(Theme.textTertiary)
                .lineSpacing(3)
            HStack(spacing: 8) {
                TextField("https://…/bge-m3/", text: $bgeURL)
                    .font(.system(size: 12, design: .monospaced))
                    .textFieldStyle(.roundedBorder)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                Button("Download") { store.download(model, customURL: bgeURL) }
                    .font(.system(size: 13, weight: .semibold))
                    .buttonStyle(.borderedProminent)
            }
        }
    }

    // ── bits ────────────────────────────────────────────────────────────────

    private func kindBadge(_ kind: LocalModel.Kind) -> some View {
        let (label, tint): (String, Color) = kind == .llm
            ? ("CHAT", Theme.accent)
            : ("EMBEDDING", Theme.user)
        return Text(label)
            .font(.system(size: 9, weight: .bold))
            .kerning(1)
            .padding(.horizontal, 6).padding(.vertical, 3)
            .background(Capsule().fill(tint.opacity(0.15)))
            .foregroundStyle(tint)
    }

    /// Route this slot's provider to MNN (on) or the remote API (off).
    private func providerToggle(_ model: LocalModel) -> some View {
        let binding = model.kind == .llm
            ? Binding(get: { state.localLLM }, set: { state.localLLM = $0 })
            : Binding(get: { state.localEmbedding }, set: { state.localEmbedding = $0 })
        return Toggle(isOn: binding) {
            Text("Use locally")
                .font(.system(size: 12))
                .foregroundStyle(Theme.textSecondary)
        }
        .tint(Theme.accent)
    }

    private func bytesString(_ b: Int64) -> String {
        let gb = Double(b) / 1_073_741_824
        return String(format: "%.1f GB", gb)
    }
}
