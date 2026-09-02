import Foundation
import MiniclawCore

/// Owns the native engine (libminiclaw_core.dylib).
///
/// Mirrors Android's EngineClient.kt 1:1 in semantics:
///   - `start()` creates the engine on a background thread (workspace
///     bootstrap is not instant); the handle is published on main.
///   - Engine events arrive on a worker thread (the engine's turn fiber);
///     they are marshalled onto the main thread before `onEvent` fires.
///   - `stop()` destroys the engine; the app does this when backgrounded
///     (like Android's onStop) and restarts on foreground.
///
/// Threading contract: all EngineClient methods are called from the main
/// thread. `engine` is only ever written on main; native calls that block
/// (create/destroy) run on a private queue, with results marshalled back.
///
/// Unlike Android there is no JNI bridge library: Swift imports the C ABI
/// directly through the MiniclawCore module (FFI/module.modulemap).
final class EngineClient {

    struct Event {
        /// "status" | "token" | "tool_start" | "tool_end" | "error" | "done"
        let type: String
        let content: String
    }

    /// Invoked on the main thread.
    var onEvent: ((Event) -> Void)?

    private(set) var isReady = false
    private(set) var isStarting = false
    private(set) var startError: String?

    /// mc_engine* — opaque to Swift. Main-thread only.
    private var engine: OpaquePointer?
    /// Bumped by stop(); a create in flight when the app backgrounds is
    /// discarded (its result never belongs to a live session).
    private var session = 0
    private let queue = DispatchQueue(label: "com.miniclaw.engine")
    private let workspaceDir: String

    init(workspaceDir: String) {
        self.workspaceDir = workspaceDir
    }

    // ── lifecycle ───────────────────────────────────────────────────────────

    /// Create and start the engine in the background. Main thread.
    func start() {
        guard engine == nil, !isStarting else { return }
        isStarting = true
        isReady = false
        let dir = workspaceDir
        let s = session
        queue.async { [weak self] in
            let h = mc_engine_create(dir, nil)
            DispatchQueue.main.async {
                guard let self, self.session == s else {
                    if h != nil { mc_engine_destroy(h) }  // stopped mid-create
                    return
                }
                self.isStarting = false
                if h == nil {
                    let err = String(cString: mc_last_error())
                    self.startError = "engine start failed: \(err)"
                    self.onEvent?(Event(type: "error", content: self.startError ?? ""))
                    return
                }
                self.engine = h
                self.isReady = true
                self.startError = nil
                self.onEvent?(Event(type: "status", content: "ready"))
            }
        }
    }

    /// Shut down the engine and release native resources. Idempotent.
    /// Main thread; the (blocking) destroy runs on the private queue.
    func stop() {
        session += 1          // invalidate any create in flight
        isStarting = false
        guard let h = engine else { return }
        engine = nil          // no new sends can see it
        isReady = false
        queue.async {
            mc_engine_destroy(h)
        }
    }

    var isRunning: Bool {
        guard let engine else { return false }
        return mc_is_running(engine) == 1
    }

    static func version() -> String {
        String(cString: mc_version())
    }

    // ── chat ────────────────────────────────────────────────────────────────

    /// Send a user message. Events stream via `onEvent` (main thread), ending
    /// with "done" (or "error"). Turns are serialized by the engine.
    func send(sessionId: String, message: String) {
        guard let engine else { return }
        mc_send_message(engine, sessionId, message, Self.cBridge,
                        Unmanaged.passUnretained(self).toOpaque())
    }

    /// C ABI event bridge. Runs on the engine's worker thread — must be fast
    /// and non-blocking; we only copy strings and hop to main. `user_data`
    /// is an unretained EngineClient (see send()); the app keeps it alive for
    /// the process lifetime, same as Android's listener object.
    private static let cBridge: mc_event_cb = { userData, typePtr, contentPtr in
        guard let userData else { return }
        let client = Unmanaged<EngineClient>.fromOpaque(userData).takeUnretainedValue()
        let type = typePtr.map { String(cString: $0) } ?? ""
        let content = contentPtr.map { String(cString: $0) } ?? ""
        DispatchQueue.main.async {
            client.onEvent?(Event(type: type, content: content))
        }
    }

    // ── config ──────────────────────────────────────────────────────────────

    /// Current config file contents (YAML), or nil if none was loaded.
    func getConfig() -> String? {
        guard let engine else { return nil }
        let ptr = mc_get_config(engine)
        defer { if let ptr { mc_free_string(ptr) } }
        return ptr.map { String(cString: $0) }
    }

    /// Write new config YAML and apply it. true on success.
    func setConfig(_ yaml: String) -> Bool {
        guard let engine else { return false }
        return mc_set_config(engine, yaml) == 0
    }

    /// Set a single setting (section/key/value). true on success.
    func setString(section: String, key: String, value: String) -> Bool {
        guard let engine else { return false }
        return mc_set_string(engine, section, key, value) == 0
    }
}
