import Foundation

/// On-device MNN models the app can download and run locally.
///
/// Files land in `<workspace>/models/<dirName>/` (see
/// `MiniclawPaths.workspaceDir`). The engine reads them from disk — no app
/// bundle involvement, so large weights never bloat the IPA.
///
/// See docs/LOCAL_MODELS.md for the full catalog and the one-time BGE-M3
/// conversion step.
struct LocalModel: Identifiable {
    enum Kind: String {
        case llm = "llm"
        case embedding = "embedding"

        /// Config section/key pair that routes this slot to MNN.
        var providerSetting: (section: String, key: String) {
            switch self {
            case .llm: return ("conversation", "provider")
            case .embedding: return ("embedding", "provider")
            }
        }
    }

    let id: String
    let kind: Kind
    let name: String
    let blurb: String
    /// Directory under <workspace>/models/
    let dirName: String
    /// Files to fetch, in order (weights last so progress feels right).
    let files: [String]
    /// HuggingFace resolve base URL. nil = not directly downloadable
    /// (requires a one-time conversion on a dev machine).
    let repoURL: String?
    /// Approximate total download size, for the UI.
    let sizeBytes: Int64

    static let catalog: [LocalModel] = [
        LocalModel(
            id: "qwen3-1.7b",
            kind: .llm,
            name: "Qwen3 1.7B (int4)",
            blurb: "Chat & summarization — runs the agent loop and memory distillation on-device.",
            dirName: "qwen3-1.7b",
            files: ["llm_config.json", "tokenizer.txt", "llm.mnn", "llm.mnn.weight"],
            repoURL: "https://huggingface.co/taobao-mnn/Qwen3-1.7B-MNN/resolve/main/",
            sizeBytes: 1_240_000_000
        ),
        LocalModel(
            id: "bge-m3",
            kind: .embedding,
            name: "BGE-M3",
            blurb: "Multilingual embeddings for the memory index. Requires a one-time conversion (see below).",
            dirName: "bge-m3",
            // embeddings_bf16.bin is the word-embedding table the engine looks
            // up before feeding the graph (required, ~512 MB).
            files: ["llm_config.json", "tokenizer.txt", "embeddings_bf16.bin", "llm.mnn", "llm.mnn.weight"],
            repoURL: nil,
            sizeBytes: 2_200_000_000
        ),
    ]

    static func model(_ id: String) -> LocalModel? {
        catalog.first { $0.id == id }
    }
}
