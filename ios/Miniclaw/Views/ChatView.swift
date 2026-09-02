import SwiftUI

/// Chat tab — native port of frontend/src/components/Chat.tsx.
/// Bubbles, activity panel (⚡ status / 🔧 tool / ✓ result), streaming
/// tokens, "Thinking…" indicator and the bottom composer.
struct ChatView: View {
    @EnvironmentObject private var state: AppState
    @State private var input = ""
    @FocusState private var inputFocused: Bool

    var body: some View {
        ZStack {
            Theme.page.ignoresSafeArea()
            if state.messages.isEmpty {
                emptyState
            } else {
                messageList
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) { composer }
    }

    // ── empty state (Chat.tsx lines 127-137) ───────────────────────────────

    private var emptyState: some View {
        VStack(spacing: 16) {
            ZStack {
                Circle()
                    .fill(Theme.card)
                    .overlay(Circle().strokeBorder(Theme.hairline, lineWidth: 1))
                    .frame(width: 72, height: 72)
                Image(systemName: "sparkle")
                    .font(.system(size: 30, weight: .light))
                    .foregroundStyle(Theme.textTertiary)
            }
            VStack(spacing: 4) {
                Text("MINICLAW ASSISTANT")
                    .font(.system(size: 11, weight: .bold))
                    .kerning(2)
                    .foregroundStyle(Theme.textTertiary)
                Text(subtitle)
                    .font(.system(size: 14, weight: .medium))
                    .foregroundStyle(Theme.textSecondary.opacity(0.7))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 40)
            }
        }
    }

    private var subtitle: String {
        if let err = state.startError { return err }
        switch (state.isReady, state.isSending) {
        case (true, false):  return "Ready to help. Ask me anything or give me a task."
        case (true, true):   return "Working on it…"
        default:             return "Starting up…"
        }
    }

    // ── message list ────────────────────────────────────────────────────────

    private var messageList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 24) {
                    ForEach(state.messages) { msg in
                        MessageRow(message: msg)
                            .id(msg.id)
                    }
                    Group {
                        if state.isSending { thinkingIndicator }
                    }
                    .id("thinking")
                }
                .padding(.horizontal, 16)
                .padding(.top, 24)
                .padding(.bottom, 16)
            }
            .onChange(of: state.messages) { _ in
                withAnimation(.easeOut(duration: 0.15)) {
                    if state.isSending {
                        proxy.scrollTo("thinking", anchor: .bottom)
                    } else if let last = state.messages.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
            .onAppear {
                if let last = state.messages.last {
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

    // ── composer (Chat.tsx input area) ──────────────────────────────────────

    private var composer: some View {
        HStack(alignment: .bottom, spacing: 8) {
            TextField("Type a message…", text: $input, axis: .vertical)
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
            && !state.isSending
    }

    private func send() {
        let text = input
        guard canSend else { return }
        input = ""
        state.send(text)
        inputFocused = true
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
