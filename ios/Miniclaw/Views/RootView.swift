import SwiftUI

/// Three tabs, matching the Tauri app's mobile layout (BottomNav.tsx):
/// Chat / Status / Settings.
struct RootView: View {
    @EnvironmentObject private var state: AppState
    @State private var tab: Tab = .chat

    enum Tab: Hashable { case chat, status, settings }

    var body: some View {
        TabView(selection: $tab) {
            ChatView()
                .tabItem { Label("Chat", systemImage: "bubble.left.and.bubble.right.fill") }
                .tag(Tab.chat)

            StatusView()
                .tabItem { Label("Status", systemImage: "waveform.path.ecg") }
                .tag(Tab.status)

            SettingsView()
                .tabItem { Label("Settings", systemImage: "gearshape.fill") }
                .tag(Tab.settings)
        }
    }
}
