import SwiftUI

@main
struct MiniclawApp: App {
    @StateObject private var state = AppState()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(state)
                .preferredColorScheme(.dark)
                .tint(Theme.accent)
                .onAppear { state.engineStart() }
        }
        .onChange(of: scenePhase) { phase in
            // Mirror Android's onStart/onStop: the engine is a heavyweight
            // in-process service (fiber pool, libuv loop, faiss index), so it
            // lives only while the app is foregrounded.
            switch phase {
            case .active:
                // Self-guarding: no-op when already running/starting.
                state.engineStart()
            case .background:
                state.engineStop()
            default:
                break
            }
        }
    }
}
