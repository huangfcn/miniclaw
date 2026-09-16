import SwiftUI
import Combine

/// Shared app paths.
enum MiniclawPaths {
    /// Application Support/miniclaw — the engine's workspace (sessions, memory,
    /// config, topics.json). Created on first access.
    static var workspaceDir: String {
        let dir = (FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first?
                   .appendingPathComponent("miniclaw", isDirectory: true))!
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.path
    }
}

/// A Slack-style "channel". `id` doubles as the engine's session id, so each
/// topic keeps its own conversation history and context on disk — chatting in
/// #news never bleeds into #travel.
struct Topic: Identifiable, Codable, Equatable {
    let id: String        // engine session id, e.g. "news"
    var name: String      // display name, e.g. "News Summary"
    var symbol: String    // SF Symbol for the channel avatar
    var colorHex: String  // channel accent color (#rrggbb)
    var blurb: String     // one-line description (list + chat header)
    var pinned: Bool
    /// How the agent should behave in this topic. Written to the workspace's
    /// topics.json and injected into the system prompt for this session, so
    /// e.g. #meetings knows it is a work journal it can analyze for patterns.
    /// Defaults to "" so topics saved before this field existed still decode.
    var purpose: String = ""

    var color: Color { Color(hex: colorHex) ?? Theme.accent }
}

extension Color {
    /// #rrggbb → Color (nil on bad input).
    init?(hex: String) {
        var s = hex.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, let v = UInt32(s, radix: 16) else { return nil }
        self.init(red: Double((v >> 16) & 0xff) / 255,
                  green: Double((v >> 8) & 0xff) / 255,
                  blue: Double(v & 0xff) / 255)
    }
}

/// Owns the topic list (Slack's channel sidebar). Persisted to UserDefaults
/// as JSON so pins and user-created topics survive relaunch, and mirrored to
/// <workspace>/topics.json so the backend can inject each topic's purpose
/// into the system prompt of that session. Seeds five built-in channels on
/// first launch.
final class TopicStore: ObservableObject {

    @Published var topics: [Topic] { didSet { save() } }

    private let defaults = UserDefaults.standard
    private let key = "topics.v1"
    /// Workspace dir (Application Support/miniclaw) — topics.json lives here.
    private let workspaceDir: String

    /// Built-in channels, mirroring the agent's life-shaped jobs.
    static let builtIns: [Topic] = [
        Topic(id: "news", name: "News Summary", symbol: "newspaper.fill",
              colorHex: "3b82f6", blurb: "Daily headlines & digest", pinned: true,
              purpose: "Give concise, factual news summaries. Structure digests by section (top stories, business, tech, world) and name your sources when you have them."),
        Topic(id: "weather", name: "Weather", symbol: "cloud.sun.fill",
              colorHex: "38bdf8", blurb: "Forecasts & what to wear", pinned: false,
              purpose: "Give practical weather answers: forecast, temperature range, what to wear, and whether outdoor plans are viable."),
        Topic(id: "finance", name: "Financial Status", symbol: "chart.line.uptrend.xyaxis",
              colorHex: "34d399", blurb: "Markets, budgets & money", pinned: true,
              purpose: "Give grounded financial information: market moves, budgeting, saving. Separate facts from opinion and note that this is not personalized investment advice."),
        Topic(id: "travel", name: "Travelling", symbol: "airplane",
              colorHex: "fb923c", blurb: "Trips, hotels & itineraries", pinned: false,
              purpose: "Research destinations, flights and hotels with concrete prices and dates. Compare options and recommend based on the user's stated preferences and budget."),
        Topic(id: "meetings", name: "Meeting Notes", symbol: "note.text",
              colorHex: "f472b6", blurb: "Notes, action items & follow-ups", pinned: false,
              purpose: "This is the user's work journal: meeting notes, decisions, action items and daily working activity. When the user asks for suggestions or advice (e.g. \"what do you suggest based on my behavior?\"), first use memory_search to recall recent entries from this topic and related work context, analyze patterns (recurring blockers, workload, follow-through on commitments, priorities), then give specific, actionable suggestions. When the user pastes raw notes, summarize them into a clean entry with decisions and action items."),
    ]

    /// Built-in ids — these can be pinned but not deleted.
    static let builtInIDs: Set<String> = Set(builtIns.map(\.id))

    init(workspaceDir: String) {
        self.workspaceDir = workspaceDir
        if let data = defaults.data(forKey: key),
           let saved = try? JSONDecoder().decode([Topic].self, from: data) {
            topics = saved
        } else {
            topics = Self.builtIns
        }
        save()
    }

    // ── lookups ─────────────────────────────────────────────────────────────

    func topic(_ id: String) -> Topic? { topics.first { $0.id == id } }

    var pinned: [Topic] { topics.filter(\.pinned) }
    var unpinned: [Topic] { topics.filter { !$0.pinned } }

    // ── mutations ───────────────────────────────────────────────────────────

    func togglePin(_ id: String) {
        guard let i = topics.firstIndex(where: { $0.id == id }) else { return }
        topics[i].pinned.toggle()
    }

    /// Create a user topic. Returns the new topic, or nil if the name is
    /// empty / would collide with an existing session id.
    @discardableResult
    func add(name: String) -> Topic? {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        // Session ids are file names on disk — keep them boring.
        let slug = trimmed.lowercased()
            .map { $0.isLetter || $0.isNumber ? $0 : "-" }
            .joined()
        let id = slug.split(separator: "-").joined(separator: "-")
        guard !id.isEmpty, !topics.contains(where: { $0.id == id }) else { return nil }
        let palette = ["a78bfa", "f472b6", "34d399", "fbbf24", "38bdf8", "fb7185"]
        let topic = Topic(id: id, name: trimmed, symbol: "hash",
                          colorHex: palette[topics.count % palette.count],
                          blurb: "Custom topic", pinned: false)
        topics.append(topic)
        return topic
    }

    func remove(_ id: String) {
        topics.removeAll { $0.id == id }
    }

    // ── persistence ─────────────────────────────────────────────────────────

    private func save() {
        guard let data = try? JSONEncoder().encode(topics) else { return }
        defaults.set(data, forKey: key)
        // Mirror into the workspace for the backend's system-prompt builder.
        let url = URL(fileURLWithPath: workspaceDir)
            .appendingPathComponent("topics.json")
        try? data.write(to: url, options: .atomic)
    }
}
