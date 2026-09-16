import Foundation
import Combine

/// Status of one on-device model slot as reported by the engine
/// (mc_local_status).
struct LocalSlotStatus: Equatable {
    var loaded = false
    var dir = ""
    var message = ""
}

/// Parsed output of EngineClient.localStatus().
struct EngineLocalStatus: Equatable {
    var llm = LocalSlotStatus()
    var embedding = LocalSlotStatus()
}

/// Downloads MNN model files into the workspace and tracks engine load
/// status. Not @MainActor-annotated (same convention as AppState): created
/// on the main thread by AppState, and all entry points are called from
/// SwiftUI views; internal async work hops to MainActor explicitly.
final class ModelStore: ObservableObject {

    struct DownloadState: Equatable {
        enum Phase: Equatable { case idle, downloading, done, failed(String) }
        var phase: Phase = .idle
        /// 0...1 progress across all files of the model.
        var progress: Double = 0
        var detail = ""   // current file / error text
    }

    @Published var downloads: [String: DownloadState] = [:]
    @Published var engineStatus = EngineLocalStatus()

    private let engine: EngineClient
    private let session: URLSession
    private var tasks: [String: URLSessionDownloadTask] = [:]

    /// Per-file plan for the in-flight download of a model.
    private struct FilePlan {
        var modelDir: URL
        var files: [(name: String, url: URL)]
        var totalBytes: Int64
        var index = 0
    }
    private var filePlan: [String: FilePlan] = [:]

    init(engine: EngineClient) {
        self.engine = engine
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForResource = 8 * 3600   // big weights, slow links
        session = URLSession(configuration: config)
    }

    // ── status ──────────────────────────────────────────────────────────────

    /// Pull model load state from the engine. Call after start / loads.
    func refreshStatus() {
        guard let json = engine.localStatus() else { return }
        guard let data = json.data(using: .utf8) else { return }
        do {
            let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any]
            func slot(_ key: String) -> LocalSlotStatus {
                guard let s = obj?[key] as? [String: Any] else { return LocalSlotStatus() }
                return LocalSlotStatus(
                    loaded: (s["loaded"] as? Bool) ?? false,
                    dir: (s["dir"] as? String) ?? "",
                    message: (s["message"] as? String) ?? "")
            }
            engineStatus = EngineLocalStatus(llm: slot("llm"),
                                             embedding: slot("embedding"))
        } catch {
            // Non-fatal: status UI just stays stale.
        }
    }

    // ── downloads ───────────────────────────────────────────────────────────

    /// True when every file of the model already exists on disk.
    func isDownloaded(_ model: LocalModel) -> Bool {
        let dir = modelsDir().appendingPathComponent(model.dirName, isDirectory: true)
        return model.files.allSatisfy {
            FileManager.default.fileExists(atPath: dir.appendingPathComponent($0).path)
        }
    }

    /// Start downloading a model's files. `customURL` overrides the catalog
    /// repo URL — used for self-hosted converted BGE-M3 builds. Note: there
    /// is no resume; a failed download restarts from the beginning.
    func download(_ model: LocalModel, customURL: String? = nil) {
        guard tasks[model.id] == nil else { return }
        guard let base = (customURL ?? model.repoURL), !base.isEmpty else {
            downloads[model.id] = DownloadState(
                phase: .failed("No download source. Convert BGE-M3 first " +
                               "(docs/LOCAL_MODELS.md) or paste a URL to your converted build."),
                progress: 0, detail: "")
            return
        }
        let dir = modelsDir().appendingPathComponent(model.dirName, isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)

        var files: [(String, URL)] = []
        for name in model.files {
            files.append((name, URL(string: base + name)!))
        }
        // Approximate total: catalog size for the weight file, ~1 MB each
        // for the small ones. Good enough for a progress bar.
        let total = model.sizeBytes + Int64(model.files.count) * 1_000_000
        filePlan[model.id] = FilePlan(modelDir: dir, files: files, totalBytes: total)
        downloads[model.id] = DownloadState(phase: .downloading, progress: 0,
                                            detail: "starting…")
        startNextFile(model.id)
    }

    func cancel(_ modelID: String) {
        tasks[modelID]?.cancel()
        tasks[modelID] = nil
        filePlan[modelID] = nil
        downloads[modelID] = DownloadState(phase: .idle, progress: 0, detail: "")
    }

    private func startNextFile(_ modelID: String) {
        guard var plan = filePlan[modelID], plan.index < plan.files.count else {
            if let d = downloads[modelID], d.phase == .downloading {
                downloads[modelID] = DownloadState(phase: .done, progress: 1,
                                                   detail: "download complete")
            }
            filePlan[modelID] = nil
            tasks[modelID] = nil
            refreshStatus()
            return
        }
        let (name, url) = plan.files[plan.index]
        let dest = plan.modelDir.appendingPathComponent(name)
        if FileManager.default.fileExists(atPath: dest.path) {
            plan.index += 1   // already present — skip
            filePlan[modelID] = plan
            startNextFile(modelID)
            return
        }

        let task = session.downloadTask(with: url) { [weak self] tempURL, _, error in
            // Completion handler runs on a URLSession queue — hop to main.
            DispatchQueue.main.async {
                self?.handleCompletion(modelID: modelID, name: name,
                                       tempURL: tempURL, error: error)
            }
        }
        // Progress: bytes written so far across this file (fires on an
        // arbitrary queue — hop to main).
        let progressObservation = task.progress.addObservation { [weak self] p in
            DispatchQueue.main.async {
                guard let self, var plan = self.filePlan[modelID] else { return }
                let doneBefore = self.bytesOnDisk(plan: plan)
                let frac = Double(min(doneBefore + p.totalUnitCount, plan.totalBytes))
                    / Double(max(plan.totalBytes, 1))
                self.downloads[modelID]?.progress = min(frac, 0.999)
            }
        }
        // Keep the observation alive until the task finishes.
        taskObservations[modelID] = progressObservation
        tasks[modelID] = task
        task.resume()
        downloads[modelID]?.detail = "downloading \(name)"
    }

    private var taskObservations: [String: ProgressObservation] = [:]

    /// Bytes already on disk for files before the current index.
    private func bytesOnDisk(plan: FilePlan) -> Int64 {
        (0..<plan.index).reduce(0) { sum, i in
            let url = plan.modelDir.appendingPathComponent(plan.files[i].name)
            guard let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
                  let size = attrs[.size] as? Int64 else { return sum }
            return sum + size
        }
    }

    private func handleCompletion(modelID: String, name: String,
                                  tempURL: URL?, error: Error?) {
        taskObservations[modelID] = nil
        guard var plan = filePlan[modelID] else { return }   // cancelled

        if let tempURL {
            let dest = plan.modelDir.appendingPathComponent(name)
            try? FileManager.default.removeItem(at: dest)
            do {
                try FileManager.default.moveItem(at: tempURL, to: dest)
            } catch {
                fail(modelID, "move failed: \(error.localizedDescription)", name)
                return
            }
        } else if let error {
            // The URL API leaves no partial file behind; retry starts over.
            fail(modelID, error.localizedDescription, name)
            return
        }

        plan.index += 1
        filePlan[modelID] = plan
        startNextFile(modelID)
    }

    private func fail(_ modelID: String, _ message: String, _ detail: String) {
        downloads[modelID] = DownloadState(phase: .failed(message), progress: 0,
                                           detail: detail)
        filePlan[modelID] = nil
        tasks[modelID] = nil
    }

    // ── engine load ─────────────────────────────────────────────────────────

    /// Ask the engine to preload a model in the background; poll status while
    /// it warms up.
    func loadInEngine(_ model: LocalModel) {
        engine.localLoad(kind: model.kind.rawValue)
        for delay in [2.0, 5.0, 10.0, 20.0, 40.0] {
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                self?.refreshStatus()
            }
        }
    }

    func modelsDir() -> URL {
        let dir = URL(fileURLWithPath: MiniclawPaths.workspaceDir)
            .appendingPathComponent("models", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
}
