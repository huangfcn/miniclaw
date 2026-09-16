import SwiftUI

/// Home tab — the Slack channel list. Pinned topics on top, the rest below;
/// tap a row to open its conversation (TopicChatView). Pin/unpin from the
/// row context menu or the chat header; "+" creates a custom topic.
struct TopicsListView: View {
    @EnvironmentObject private var state: AppState
    @EnvironmentObject private var topics: TopicStore

    /// Push a topic id onto the enclosing NavigationStack (owned by RootView).
    var openTopic: (String) -> Void = { _ in }

    @State private var showAdd = false
    @State private var newName = ""
    @State private var addError: String?

    var body: some View {
        ZStack {
            Theme.page.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    header

                    if !topics.pinned.isEmpty {
                        section("Pinned", icon: "pin.fill", items: topics.pinned)
                    }
                    if !topics.unpinned.isEmpty {
                        section("Topics", icon: "hash", items: topics.unpinned)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.top, 24)
                .padding(.bottom, 32)
            }
        }
        .alert("New topic", isPresented: $showAdd) {
            TextField("Name (e.g. Health & Fitness)", text: $newName)
                .font(.system(size: 14))
            if let err = addError {
                Text(err)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(Theme.error)
            }
            Button("Cancel", role: .cancel) { resetAdd() }
            Button("Create") { createTopic() }
        } message: {
            Text("Each topic is its own conversation with the agent — like a Slack channel.")
                .font(.system(size: 12))
                .foregroundStyle(Theme.textSecondary)
        }
    }

    // ── header ──────────────────────────────────────────────────────────────

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: "sparkle")
                .font(.system(size: 16, weight: .medium))
                .foregroundStyle(Theme.accent)
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(Theme.accent.opacity(0.12)))

            VStack(alignment: .leading, spacing: 1) {
                Text("MINICLAW")
                    .font(.system(size: 18, weight: .bold))
                    .kerning(2)
                    .foregroundStyle(Theme.textPrimary)
                HStack(spacing: 6) {
                    Circle()
                        .fill(statusColor)
                        .frame(width: 7, height: 7)
                    Text(statusLabel)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(Theme.textSecondary)
                }
            }

            Spacer()

            Button {
                addError = nil
                newName = ""
                showAdd = true
            } label: {
                Image(systemName: "plus")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(Theme.accent)
                    .frame(width: 34, height: 34)
                    .background(RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(Theme.card))
                    .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .strokeBorder(Theme.hairline, lineWidth: 1))
            }
            .buttonStyle(.plain)
        }
    }

    private var statusColor: Color {
        if state.startError != nil { return Theme.error }
        if state.isReady { return Theme.ok }
        return Theme.warn
    }

    private var statusLabel: String {
        if let err = state.startError { return "engine error" }
        if state.isBusy { return "working…" }
        if state.isReady { return "online" }
        return "starting…"
    }

    // ── sections + rows ─────────────────────────────────────────────────────

    private func section(_ title: String, icon: String, items: [Topic]) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: icon)
                    .font(.system(size: 10, weight: .bold))
                Text(title.uppercased())
                    .font(.system(size: 10, weight: .bold))
                    .kerning(1.5)
            }
            .foregroundStyle(Theme.textTertiary)

            VStack(spacing: 6) {
                ForEach(items) { topic in
                    NavigationLink(value: topic.id) {
                        TopicRow(topic: topic,
                                 streaming: state.isStreaming(topic.id))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    private func resetAdd() {
        newName = ""
        addError = nil
    }

    private func createTopic() {
        guard let created = topics.add(name: newName) else {
            addError = "That name is taken or empty."
            return
        }
        showAdd = false
        resetAdd()
        // Open the fresh channel right away.
        openTopic(created.id)
    }
}

// ── one channel row ─────────────────────────────────────────────────────────

struct TopicRow: View {
    @EnvironmentObject private var topics: TopicStore
    let topic: Topic
    var streaming: Bool = false

    var body: some View {
        HStack(spacing: 12) {
            TopicAvatar(topic: topic, size: 40)

            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 3) {
                    Text("#")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundStyle(topic.color)
                    Text(topic.name)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Theme.textPrimary)
                        .lineLimit(1)
                }
                Text(topic.blurb)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(Theme.textSecondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 8)

            if streaming {
                ProgressView()
                    .controlSize(.small)
                    .tint(topic.color)
            } else if topic.pinned {
                Image(systemName: "pin.fill")
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.textTertiary)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background(
            RoundedRectangle(cornerRadius: 16, style: .continuous)
                .fill(Theme.card)
                .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .strokeBorder(Theme.hairline, lineWidth: 1))
        )
        .contentShape(Rectangle())
        .contextMenu {
            Button {
                topics.togglePin(topic.id)
            } label: {
                Label(topic.pinned ? "Unpin" : "Pin",
                      systemImage: topic.pinned ? "pin.slash" : "pin")
            }
            if !TopicStore.builtInIDs.contains(topic.id) {
                Button(role: .destructive) {
                    topics.remove(topic.id)
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            }
        }
    }
}
