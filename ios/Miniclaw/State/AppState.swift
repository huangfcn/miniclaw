import Foundation
import Combine

/// One chat bubble. Mirrors the Message interface in frontend Chat.tsx.
struct ChatMessage: Identifiable, Equatable {
    enum Role { case user, agent }
    let id = UUID()
    let role: Role
    var content: String = ""
    /// Activity lines under an agent bubble: "⚡ status", "🔧 tool", "✓ result".
    var activities: [String] = []
    var isError = false
}

/// App-wide observable state: engine lifecycle, per-topic chat streams,
/// settings. Owns the EngineClient (like Android's MainActivity owning
/// EngineClient).
///
/// Topics: each Slack-style topic is one engine session (`Topic.id`). The
/// engine serializes turns globally — at most one turn runs at a time and
/// later sends are queued — so `pendingTopics` is a FIFO of sessions with
/// in-flight turns; its first element is always the topic currently
/// receiving events. Events carry no session id (C ABI), which is why the
/// queue exists.
///
/// Not @MainActor-annotated: the C event bridge assigns a plain closure to
/// EngineClient.onEvent that calls apply(); under Swift 5 language mode a
/// nonisolated closure cannot call @MainActor methods. All callers are
/// SwiftUI views (main thread), and EngineClient marshals engine events to
/// main before onEvent fires — same contract as the Android port.
final class AppState: ObservableObject {

    // ── engine ──────────────────────────────────────────────────────────────
    @Published var isReady = false
    @Published var startError: String?
    @Published var statusText = "starting…"
    @Published var readyAt: Date?
    let version: String
    let workspaceDir: String

    // ── chat (per topic) ────────────────────────────────────────────────────
    /// Messages keyed by topic id (= engine session id).
    @Published var messagesByTopic: [String: [ChatMessage]] = [:]
    /// FIFO of topics with in-flight turns; first = currently streaming.
    @Published private(set) var pendingTopics: [String] = []

    // ── settings (persisted like Android's SharedPreferences) ──────────────
    @Published var endpoint: String { didSet { defaults.set(endpoint, forKey: "endpoint") } }
    @Published var model: String    { didSet { defaults.set(model, forKey: "model") } }
    @Published var apiKey: String   { didSet { defaults.set(apiKey, forKey: "api_key") } }
    @Published var braveKey: String { didSet { defaults.set(braveKey, forKey: "brave_api_key") } }

    /// Route chat / embedding through the on-device MNN model instead of
    /// the remote OpenAI-compatible provider (Models tab toggles).
    @Published var localLLM: Bool {
        didSet { applyLocalProvider(.llm, localLLM) }
    }
    @Published var localEmbedding: Bool {
        didSet { applyLocalProvider(.embedding, localEmbedding) }
    }

    /// Raw config.yaml for the advanced editor (Settings tab).
    @Published var yamlText: String = ""
    /// Snapshot of the last loaded/saved content; dirty = text != snapshot.
    @Published var yamlLoaded: String = ""
    @Published var saveNotice: String?

    /// Local MNN model downloads + engine load status (Models tab).
    let models: ModelStore
    private let client: EngineClient
    private let defaults = UserDefaults.standard

    init() {
        workspaceDir = MiniclawPaths.workspaceDir

        endpoint = defaults.string(forKey: "endpoint") ?? ""
        model    = defaults.string(forKey: "model") ?? ""
        apiKey   = defaults.string(forKey: "api_key") ?? ""
        braveKey = defaults.string(forKey: "brave_api_key") ?? ""
        localLLM       = defaults.string(forKey: Self.localProviderKey(.llm)) == "mnn"
        localEmbedding = defaults.string(forKey: Self.localProviderKey(.embedding)) == "mnn"

        client = EngineClient(workspaceDir: workspaceDir)
        version = EngineClient.version()
        models = ModelStore(engine: client)
        client.onEvent = { [weak self] event in
            self?.apply(event)
        }
    }

    // ── lifecycle (driven from scenePhase) ─────────────────────────────────

    func engineStart() {
        guard !client.isReady && !client.isStarting else { return }
        startError = nil
        isReady = false
        readyAt = nil
        statusText = "starting…"
        client.start()
        // Mirror the client's outcome once create finishes (it may take a
        // while: workspace bootstrap + index load), then re-apply the
        // persisted LLM settings.
        Task { @MainActor in
            // Exits when create finishes (ready or error) or the app
            // backgrounds mid-create (stop() clears isStarting).
            while client.isStarting && !client.isReady && client.startError == nil {
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            if let err = client.startError {
                self.startError = err
                statusText = "error"
                return
            }
            guard client.isReady else { return }   // stopped mid-create
            isReady = true
            startError = nil
            readyAt = Date()
            statusText = "ready"
            applySavedSettings()
            models.refreshStatus()
        }
    }

    func engineStop() {
        client.stop()
        isReady = false
        startError = nil
        readyAt = nil
        statusText = "stopped"
        // In-flight turns die with the engine; drop them so no topic shows
        // a stale "thinking…" spinner after relaunch.
        pendingTopics = []
    }

    private func applySavedSettings() {
        if !endpoint.isEmpty { client.setString(section: "conversation", key: "endpoint", value: endpoint) }
        if !model.isEmpty    { client.setString(section: "conversation", key: "model", value: model) }
        if !apiKey.isEmpty   { client.setString(section: "conversation", key: "api_key", value: apiKey) }
        if !braveKey.isEmpty { client.setString(section: "web", key: "brave_api_key", value: braveKey) }
        // Re-apply local provider toggles (didSet doesn't fire during init).
        if localLLM       { applyLocalProvider(.llm, true) }
        if localEmbedding { applyLocalProvider(.embedding, true) }
    }

    static func localProviderKey(_ kind: LocalModel.Kind) -> String {
        "local_provider_\(kind.rawValue)"
    }

    /// Push a provider toggle to the running engine (didSet may fire before
    /// the engine exists — setString is a safe no-op then; applySavedSettings
    /// re-applies both toggles on next start).
    private func applyLocalProvider(_ kind: LocalModel.Kind, _ enabled: Bool) {
        let (section, key) = kind.providerSetting
        client.setString(section: section, key: key, value: enabled ? "mnn" : "openai")
    }

    /// Persist + apply a structured setting immediately (Android dialog parity).
    func saveStructuredSettings() {
        applySavedSettings()
        flash("saved")
    }

    // ── chat ────────────────────────────────────────────────────────────────

    var isBusy: Bool { !pendingTopics.isEmpty }

    /// Session files on disk (<workspace>/sessions/*.jsonl) — one per topic,
    /// since each topic's id is used verbatim as the backend session key.
    func sessionFiles() -> [String] {
        let dir = (workspaceDir as NSString).appendingPathComponent("sessions")
        guard let names = try? FileManager.default.contentsOfDirectory(
            atPath: dir, includingPropertiesForKeys: nil) else { return [] }
        return names.filter { $0.hasSuffix(".jsonl") }.sorted()
    }

    /// Messages for one topic (empty array, never a nil lookup).
    func messages(for topicID: String) -> [ChatMessage] {
        messagesByTopic[topicID] ?? []
    }

    /// True while this topic has a turn in flight (streaming or queued).
    func isStreaming(_ topicID: String) -> Bool {
        pendingTopics.contains(topicID)
    }

    func send(_ text: String, to topicID: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, !isStreaming(topicID), client.isReady else { return }

        var msgs = messagesByTopic[topicID, default: []]
        msgs.append(ChatMessage(role: .user, content: trimmed))
        msgs.append(ChatMessage(role: .agent))
        messagesByTopic[topicID] = msgs
        pendingTopics.append(topicID)
        statusText = "thinking…"
        client.send(sessionId: topicID, message: trimmed)
    }

    /// Port of the onEvent switch in frontend/src/components/Chat.tsx,
    /// routed to the topic at the head of the turn queue.
    private func apply(_ e: EngineClient.Event) {
        guard let topicID = pendingTopics.first else { return }
        switch e.type {
        case "status":
            statusText = e.content.isEmpty ? statusText : e.content
            if !e.content.isEmpty {
                mutateAgentBubble(topicID) { $0.activities.append("⚡ \(e.content)") }
            }
        case "token":
            mutateAgentBubble(topicID) { $0.content += e.content }
        case "tool_start":
            mutateAgentBubble(topicID) { $0.activities.append("🔧 \(e.content)") }
        case "tool_end":
            let first = e.content.split(separator: "\n").first.map(String.init) ?? ""
            mutateAgentBubble(topicID) { $0.activities.append("✓ \(String(first.prefix(80)))") }
        case "error":
            mutateAgentBubble(topicID) { bubble in
                bubble.isError = true
                if bubble.content.isEmpty {
                    bubble.content = e.content
                } else if !e.content.isEmpty {
                    bubble.activities.append("⚠ \(e.content)")
                }
            }
            statusText = "error"
        case "done":
            statusText = pendingTopics.count > 1 ? "thinking…" : "ready"
        default:
            break
        }
        // "done"/"error" are the terminal events (exactly one per turn, in
        // order — turns are serialized), so release this topic's slot.
        if e.type == "done" || e.type == "error" {
            pendingTopics.removeFirst()
        }
    }

    /// Apply `mutate` to the live agent bubble for `topicID`, creating one
    /// if missing (defensive, mirrors Chat.tsx). No-op if there is none.
    private func mutateAgentBubble(_ topicID: String,
                                   _ mutate: (inout ChatMessage) -> Void) {
        var msgs = messagesByTopic[topicID] ?? []
        if let i = msgs.lastIndex(where: { $0.role == .agent }) {
            // The bubble appended in send() is the last agent one; a topic
            // can't stream two turns at once, so it's always the live one.
            mutate(&msgs[i])
        } else {
            var bubble = ChatMessage(role: .agent)
            mutate(&bubble)
            msgs.append(bubble)
        }
        messagesByTopic[topicID] = msgs
    }

    // ── settings / config yaml ─────────────────────────────────────────────

    func loadYaml() {
        let y = client.getConfig() ?? ""
        yamlText = y
        yamlLoaded = y
    }

    func saveYaml() {
        guard client.setConfig(yamlText) else { flash("save failed"); return }
        yamlLoaded = yamlText
        flash("saved")
    }

    private func flash(_ text: String) {
        saveNotice = text
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            if saveNotice == text { saveNotice = nil }
        }
    }
}
