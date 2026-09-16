import SwiftUI

/// Four tabs, Slack-style: Home (topic list) / Models / Status / Settings.
/// Home is a NavigationStack so tapping a topic pushes its channel view;
/// the path is owned here so "Create topic" can open the fresh channel.
struct RootView: View {
    @EnvironmentObject private var state: AppState
    @EnvironmentObject private var topics: TopicStore

    @State private var tab: Tab = .home
    @State private var path: [String] = []

    enum Tab: Hashable { case home, models, status, settings }

    var body: some View {
        TabView(selection: $tab) {
            NavigationStack(path: $path) {
                TopicsListView(openTopic: { id in path.append(id) })
                    .navigationDestination(for: String.self) { id in
                        if let t = topics.topic(id) {
                            TopicChatView(topic: t)
                                .navigationBarHidden(true)
                        }
                    }
            }
            .tabItem { Label("Home", systemImage: "number") }
            .tag(Tab.home)

            LocalModelsView(store: state.models)
                .tabItem { Label("Models", systemImage: "cpu.fill") }
                .tag(Tab.models)

            StatusView()
                .tabItem { Label("Status", systemImage: "waveform.path.ecg") }
                .tag(Tab.status)

            SettingsView()
                .tabItem { Label("Settings", systemImage: "gearshape.fill") }
                .tag(Tab.settings)
        }
    }
}
