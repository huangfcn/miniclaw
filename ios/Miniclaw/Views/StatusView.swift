import SwiftUI

/// Status tab — native port of the mobile Monitoring view, extended with
/// engine details (version, model, endpoint, workspace, uptime).
struct StatusView: View {
    @EnvironmentObject private var state: AppState

    var body: some View {
        ZStack {
            Theme.page.ignoresSafeArea()
            ScrollView {
                VStack(spacing: 16) {
                    Text("Engine Status")
                        .font(.system(size: 22, weight: .bold))
                        .foregroundStyle(Theme.textPrimary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.top, 24)

                    statusCard
                    // 1 Hz re-render keeps the uptime row ticking while idle.
                    TimelineView(.periodic(from: .now, by: 1)) { _ in
                        infoCard
                    }
                    privacyCard

                    if let err = state.startError {
                        errorCard(err)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 32)
            }
        }
    }

    // ── running / starting / error card (Monitoring.tsx mobile variant) ────

    private var statusCard: some View {
        let (tint, title, icon): (Color, String, String) = {
            if let err = state.startError { return (Theme.error, "Engine failed to start", "xmark.octagon.fill") }
            if state.isReady { return (Theme.ok, "Engine running", "checkmark.seal.fill") }
            return (Theme.warn, "Engine starting…", "gearshape.arrow.triangle.2.circlepath")
        }()

        return VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: icon)
                    .font(.system(size: 18))
                    .foregroundStyle(tint)
                Text(title)
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(tint)
            }
            Text("On iOS the engine runs inside the app (no separate process). It starts automatically when the app launches and stops when the app is backgrounded.")
                .font(.system(size: 12))
                .foregroundStyle(Theme.textSecondary)
                .lineSpacing(3)
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(tint.opacity(0.05))
                .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .strokeBorder(tint.opacity(0.2), lineWidth: 1))
        )
    }

    // ── engine details ──────────────────────────────────────────────────────

    private var infoCard: some View {
        VStack(spacing: 0) {
            row("Version", state.version)
            divider
            row("Status", state.statusText)
            divider
            row("Uptime", uptime)
            divider
            row("Model", state.model.isEmpty ? "default" : state.model)
            divider
            row("Endpoint", state.endpoint.isEmpty ? "default" : state.endpoint, mono: true)
            divider
            row("Workspace", state.workspaceDir, mono: true)
        }
        .background(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(Theme.card)
                .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .strokeBorder(Theme.hairline, lineWidth: 1))
        )
    }

    private var uptime: String {
        guard let start = state.readyAt else { return "—" }
        let s = max(0, Int(Date().timeIntervalSince(start)))
        return String(format: "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60)
    }

    private func row(_ label: String, _ value: String, mono: Bool = false) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(label.uppercased())
                .font(.system(size: 10, weight: .bold))
                .kerning(1.5)
                .foregroundStyle(Theme.textTertiary)
                .frame(width: 92, alignment: .leading)
            Text(value)
                .font(.system(size: 13, weight: .medium, design: mono ? .monospaced : .default))
                .foregroundStyle(Theme.textPrimary)
                .lineLimit(2)
                .truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
    }

    private var divider: some View {
        Rectangle()
            .fill(Theme.hairline)
            .frame(height: 1)
            .padding(.leading, 20)
    }

    // ── privacy card (Monitoring.tsx mobile variant) ────────────────────────

    private var privacyCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Privacy", systemImage: "hand.raised.fill")
                .font(.system(size: 10, weight: .bold))
                .kerning(1.5)
                .foregroundStyle(Theme.textTertiary)
            Text("Background tasks (summarization, embeddings, memory distillation) can run on private local models pointed at in-app endpoints via Settings → config.yaml. Conversation quality uses your configured remote API.")
                .font(.system(size: 12))
                .foregroundStyle(Theme.textSecondary)
                .lineSpacing(3)
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

    private func errorCard(_ message: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(Theme.error)
            Text(message)
                .font(.system(size: 13))
                .foregroundStyle(Theme.error)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(Theme.error.opacity(0.1))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(Theme.error.opacity(0.2), lineWidth: 1))
        )
    }
}
