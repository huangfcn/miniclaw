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

/// App-wide observable state: engine lifecycle, chat stream, settings.
/// Owns the EngineClient (like Android's MainActivity owning EngineClient).
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

    // ── chat ────────────────────────────────────────────────────────────────
    @Published var messages: [ChatMessage] = []
    @Published var isSending = false

    // ── settings (persisted like Android's SharedPreferences) ──────────────
    @Published var endpoint: String { didSet { defaults.set(endpoint, forKey: "endpoint") } }
    @Published var model: String    { didSet { defaults.set(model, forKey: "model") } }
    @Published var apiKey: String   { didSet { defaults.set(apiKey, forKey: "api_key") } }
    @Published var braveKey: String { didSet { defaults.set(braveKey, forKey: "brave_api_key") } }

    /// Raw config.yaml for the advanced editor (Settings tab).
    @Published var yamlText: String = ""
    /// Snapshot of the last loaded/saved content; dirty = text != snapshot.
    @Published var yamlLoaded: String = ""
    @Published var saveNotice: String?

    private let client: EngineClient
    private let defaults = UserDefaults.standard
    private var streamingIndex = -1   // agent bubble currently receiving events

    init() {
        let dir = (FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first?
                   .appendingPathComponent("miniclaw", isDirectory: true))!
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        workspaceDir = dir.path

        endpoint = defaults.string(forKey: "endpoint") ?? ""
        model    = defaults.string(forKey: "model") ?? ""
        apiKey   = defaults.string(forKey: "api_key") ?? ""
        braveKey = defaults.string(forKey: "brave_api_key") ?? ""

        client = EngineClient(workspaceDir: workspaceDir)
        version = EngineClient.version()
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
        }
    }

    func engineStop() {
        client.stop()
        isReady = false
        startError = nil
        readyAt = nil
        statusText = "stopped"
    }

    private func applySavedSettings() {
        if !endpoint.isEmpty { client.setString(section: "conversation", key: "endpoint", value: endpoint) }
        if !model.isEmpty    { client.setString(section: "conversation", key: "model", value: model) }
        if !apiKey.isEmpty   { client.setString(section: "conversation", key: "api_key", value: apiKey) }
        if !braveKey.isEmpty { client.setString(section: "web", key: "brave_api_key", value: braveKey) }
    }

    /// Persist + apply a structured setting immediately (Android dialog parity).
    func saveStructuredSettings() {
        applySavedSettings()
        flash("saved")
    }

    // ── chat ────────────────────────────────────────────────────────────────

    func send(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, !isSending, client.isReady else { return }

        messages.append(ChatMessage(role: .user, content: trimmed))
        messages.append(ChatMessage(role: .agent))
        streamingIndex = messages.count - 1
        isSending = true
        statusText = "thinking…"
        client.send(sessionId: "main", message: trimmed)
    }

    /// Port of the onEvent switch in frontend/src/components/Chat.tsx.
    private func apply(_ e: EngineClient.Event) {
        switch e.type {
        case "status":
            statusText = e.content.isEmpty ? statusText : e.content
            if !e.content.isEmpty, let i = streamingAgentIndex() {
                messages[i].activities.append("⚡ \(e.content)")
            }
        case "token":
            if let i = streamingAgentIndex(createIfNeeded: true) {
                messages[i].content += e.content
            }
        case "tool_start":
            if let i = streamingAgentIndex(createIfNeeded: true) {
                messages[i].activities.append("🔧 \(e.content)")
            }
        case "tool_end":
            if let i = streamingAgentIndex(createIfNeeded: true) {
                let first = e.content.split(separator: "\n").first.map(String.init) ?? ""
                messages[i].activities.append("✓ \(String(first.prefix(80)))")
            }
        case "error":
            if let i = streamingAgentIndex(createIfNeeded: true) {
                messages[i].isError = true
                if messages[i].content.isEmpty {
                    messages[i].content = e.content
                } else if !e.content.isEmpty {
                    messages[i].activities.append("⚠ \(e.content)")
                }
            }
            statusText = "error"
        case "done":
            isSending = false
            streamingIndex = -1
            statusText = "ready"
        default:
            break
        }
    }

    /// Index of the bubble currently receiving agent events; creates one if
    /// the stream starts before we appended (defensive, mirrors Chat.tsx).
    private func streamingAgentIndex(createIfNeeded: Bool = false) -> Int? {
        if streamingIndex >= 0 && streamingIndex < messages.count,
           messages[streamingIndex].role == .agent {
            return streamingIndex
        }
        guard createIfNeeded else { return nil }
        messages.append(ChatMessage(role: .agent))
        streamingIndex = messages.count - 1
        return streamingIndex
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
