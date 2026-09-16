import SwiftUI

/// One topic's conversation — the Slack "channel view": header with the
/// channel name + description and a pin toggle, message list (bubbles,
/// activity panel, streaming tokens, Thinking… indicator), bottom composer.
///
/// Replaces the old single-session ChatView; the engine session id is the
/// topic id, so each topic keeps its own history on disk.
struct TopicChatView: View {
    @EnvironmentObject private var state: AppState
    @EnvironmentObject private var topics: TopicStore

    let topic: Topic
    @State private var input = ""
    @FocusState private var inputFocused: Bool

    /// Live topic (so pin toggles / renames reflect without re-navigation).
    private var liveTopic: Topic { topics.topic(topic.id) ?? topic }
    private var messages: [ChatMessage] { state.messages(for: topic.id) }
    private var streaming: Bool { state.isStreaming(topic.id) }

    var body: some View {
        ZStack {
            Theme.page.ignoresSafeArea()
            if messages.isEmpty {
                emptyState
            } else {
                messageList
            }
        }
        .safeAreaInset(edge: .top, spacing: 0) { header }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            VStack(spacing: 0) {
                if input.isEmpty && !streaming {
                    suggestionRow
                        .padding(.horizontal, 12)
                        .padding(.top, 8)
                }
                composer
            }
        }
    }

    // ── suggested prompts (fill the composer; user taps send) ─────────────

    private var suggestions: [String] {
        switch topic.id {
        case "news":     return ["Summarize today's top stories",
                                 "What's moving in tech today?"]
        case "weather":  return ["Forecast for this week",
                                 "What should I wear today?"]
        case "finance":  return ["How are the markets doing?",
                                 "Review my budget"]
        case "travel":   return ["Plan a weekend trip under $500",
                                 "Compare hotels in Tokyo"]
        case "meetings": return ["What do you suggest based on my behavior?",
                                 "Summarize my open action items",
                                 "What patterns do you see in my work this week?"]
        default:         return ["What can I ask you here?"]
        }
    }

    private var suggestionRow: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(suggestions, id: \.self) { s in
                    Button {
                        input = s
                        inputFocused = true
                    } label: {
                        Text(s)
                            .font(.system(size: 12, weight: .medium))
                            .foregroundStyle(Theme.textSecondary)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 7)
                            .background(Capsule().fill(Theme.card))
                            .overlay(Capsule().strokeBorder(Theme.hairline, lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    // ── channel header (Slack: #name + description) ────────────────────────

    private var header: some View {
        HStack(spacing: 12) {
            TopicAvatar(topic: liveTopic, size: 34)

            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 3) {
                    Text("#")
                        .font(.system(size: 16, weight: .bold))
                        .foregroundStyle(liveTopic.color)
                    Text(liveTopic.name)
                        .font(.system(size: 16, weight: .bold))
                        .foregroundStyle(Theme.textPrimary)
                        .lineLimit(1)
                }
                Text(liveTopic.blurb)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(Theme.textSecondary)
                    .lineLimit(1)
            }

            Spacer()

            Button {
                topics.togglePin(topic.id)
            } label: {
                Image(systemName: liveTopic.pinned ? "pin.fill" : "pin")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(liveTopic.pinned ? liveTopic.color : Theme.textTertiary)
                    .frame(width: 34, height: 34)
                    .background(RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(Theme.card))
                    .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .strokeBorder(Theme.hairline, lineWidth: 1))
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Theme.page.opacity(0.98))
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.hairline).frame(height: 1)
        }
    }

    // ── empty state ─────────────────────────────────────────────────────────

    private var emptyState: some View {
        VStack(spacing: 16) {
            TopicAvatar(topic: liveTopic, size: 64)
            VStack(spacing: 5) {
                Text("#\(liveTopic.name)")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(Theme.textPrimary)
                Text(liveTopic.blurb)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(Theme.textSecondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 40)
                Text(emptyHint)
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.textTertiary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 56)
            }
        }
    }

    private var emptyHint: String {
        if let err = state.startError { return err }
        if !state.isReady { return "Starting up…" }
        switch topic.id {
        case "news":     return "Ask for today's headlines, a topic deep-dive, or set up a daily digest."
        case "weather":  return "Ask about today's forecast, this weekend, or what to pack."
        case "finance":  return "Check market moves, review your budget, or plan a purchase."
        case "travel":   return "Research destinations, compare hotels, or build an itinerary."
        case "meetings": return "Paste raw notes and get a clean summary with action items."
        default:         return "Say hello — this topic keeps its own conversation history."
        }
    }

    // ── message list ────────────────────────────────────────────────────────

    private var messageList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 24) {
                    ForEach(messages) { msg in
                        MessageRow(message: msg)
                            .id(msg.id)
                    }
                    Group {
                        if streaming { thinkingIndicator }
                    }
                    .id("thinking")
                }
                .padding(.horizontal, 16)
                .padding(.top, 20)
                .padding(.bottom, 16)
            }
            .onChange(of: state.messagesByTopic[topic.id]) { _ in
                withAnimation(.easeOut(duration: 0.15)) {
                    if streaming {
                        proxy.scrollTo("thinking", anchor: .bottom)
                    } else if let last = messages.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
            .onAppear {
                if let last = messages.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
    }

    private var thinkingIndicator: some View {
        HStack(spacing: 10) {
            ProgressView()
                .controlSize(.small)
                .tint(Theme.textTertiary)
            Text("THINKING…")
                .font(.system(size: 11, weight: .bold))
                .kerning(2)
                .foregroundStyle(Theme.textTertiary)
        }
        .padding(.leading, 4)
    }

    // ── composer ────────────────────────────────────────────────────────────

    private var composer: some View {
        HStack(alignment: .bottom, spacing: 8) {
            TextField("Message #\(liveTopic.name)", text: $input, axis: .vertical)
                .lineLimit(1...5)
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(Theme.textPrimary)
                .tint(Theme.accent)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .focused($inputFocused)
                .onSubmit(send)

            Button(action: send) {
                Image(systemName: "arrow.up")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(canSend ? Color.white : Theme.textTertiary)
                    .frame(width: 34, height: 34)
                    .background(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .fill(canSend ? Theme.accentDim : Theme.cardAlt)
                    )
            }
            .disabled(!canSend)
        }
        .padding(8)
        .background(
            RoundedRectangle(cornerRadius: 20, style: .continuous)
                .fill(Theme.card)
                .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous)
                    .strokeBorder(inputFocused ? Theme.accent.opacity(0.5) : Theme.hairline,
                                  lineWidth: 1))
        )
        .padding(.horizontal, 12)
        .padding(.top, 8)
        .padding(.bottom, 6)
        .background(Theme.page.opacity(0.98))
    }

    private var canSend: Bool {
        state.isReady
            && !input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && !streaming
    }

    private func send() {
        let text = input
        guard canSend else { return }
        input = ""
        state.send(text, to: topic.id)
        inputFocused = true
    }
}

// ── shared channel avatar (list rows + chat header + empty state) ──────────

struct TopicAvatar: View {
    let topic: Topic
    var size: CGFloat = 32

    var body: some View {
        Image(systemName: topic.symbol)
            .font(.system(size: size * 0.46, weight: .medium))
            .foregroundStyle(topic.color)
            .frame(width: size, height: size)
            .background(RoundedRectangle(cornerRadius: size * 0.31, style: .continuous)
                .fill(topic.color.opacity(0.12)))
            .overlay(RoundedRectangle(cornerRadius: size * 0.31, style: .continuous)
                .strokeBorder(topic.color.opacity(0.25), lineWidth: 1))
    }
}

// ── one bubble row ──────────────────────────────────────────────────────────

struct MessageRow: View {
    let message: ChatMessage

    var body: some View {
        switch message.role {
        case .user:  userRow
        case .agent: agentRow
        }
    }

    private var userRow: some View {
        HStack(alignment: .top, spacing: 12) {
            Spacer(minLength: 40)
            bubble(message.content, fill: Theme.cardAlt, border: Theme.hairline,
                   textColor: Theme.textPrimary)
                .frame(maxWidth: 340, alignment: .trailing)
            avatar("person.fill", tint: Theme.user, bg: Theme.user.opacity(0.1),
                   border: Theme.user.opacity(0.2))
        }
    }

    private var agentRow: some View {
        HStack(alignment: .top, spacing: 12) {
            avatar(message.isError ? "exclamationmark.triangle.fill" : "sparkle",
                   tint: message.isError ? Theme.error : Theme.accent,
                   bg: (message.isError ? Theme.error : Theme.accent).opacity(0.1),
                   border: (message.isError ? Theme.error : Theme.accent).opacity(0.2))

            VStack(alignment: .leading, spacing: 8) {
                if !message.activities.isEmpty {
                    activitiesPanel
                }
                if !message.content.isEmpty {
                    bubble(message.content,
                           fill: message.isError ? Theme.error.opacity(0.1) : Theme.card,
                           border: message.isError ? Theme.error.opacity(0.25) : Theme.hairline,
                           textColor: message.isError ? Theme.error : Theme.textPrimary)
                        .frame(maxWidth: 340, alignment: .leading)
                }
            }
            Spacer(minLength: 12)
        }
    }

    private var activitiesPanel: some View {
        ScrollView(.vertical) {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(message.activities.enumerated()), id: \.offset) { _, line in
                    HStack(spacing: 6) {
                        Image(systemName: "wrench")
                            .font(.system(size: 9))
                            .opacity(0.5)
                        Text(line)
                            .font(.system(size: 11, design: .monospaced))
                            .lineLimit(2)
                    }
                    .foregroundStyle(Theme.textTertiary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxHeight: 128)
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(Theme.card.opacity(0.6))
                .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(Theme.hairline, lineWidth: 1))
        )
    }

    private func bubble(_ text: String, fill: Color, border: Color, textColor: Color) -> some View {
        Text(text)
            .font(.system(size: 15, weight: .medium))
            .lineSpacing(3)
            .foregroundStyle(textColor)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .fill(fill)
                    .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                        .strokeBorder(border, lineWidth: 1))
            )
    }

    private func avatar(_ symbol: String, tint: Color, bg: Color, border: Color) -> some View {
        Image(systemName: symbol)
            .font(.system(size: 15, weight: .medium))
            .foregroundStyle(tint)
            .padding(8)
            .frame(width: 32, height: 32)
            .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(bg))
            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(border, lineWidth: 1))
    }
}
