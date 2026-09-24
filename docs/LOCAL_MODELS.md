# Local Models (MNN Inference)

Miniclaw can run **fully local** LLM and embedding inference on-device using
[Alibaba MNN](https://github.com/alibaba/MNN). This document covers the design,
the model catalog, setup, and configuration.

## What runs locally

| Role | Default remote path | Local (MNN) path |
|---|---|---|
| **Summarization / memory distillation** (L1→L2 daily logs, L2→L3 consolidation) | OpenAI-compatible chat endpoint (`memory.distillation_*`) | **Qwen3-1.7B** via MNN (`provider: mnn`) |
| **Embedding generation** (memory index, hybrid search) | OpenAI-compatible `/v1/embeddings` (`embedding.*`) | **BGE-M3** via MNN (`provider: mnn`) |
| Main conversation | OpenAI-compatible chat endpoint (`conversation.*`) | Also possible with `conversation.provider: mnn` (same Qwen model) |

The design goal is: **summarization and embeddings work with zero network
access**. The main conversation can stay on a remote API if you prefer higher
quality, or also move local.

## Architecture

```
┌────────────────────────── libminiclaw_core ──────────────────────────┐
│                                                                      │
│  Agent::call_llm(messages, tools, on_event, model, endpoint, provider)│
│    ├─ provider == "mnn"  → MnnInference::chat()                      │
│    │                        · worker std::thread (MNN is CPU/GPU-bound)│
│    │                        · tokens streamed via on_event("token")   │
│    │                        · fiber resumed on the owning loop thread │
│    └─ otherwise          → existing curl/SSE HTTP path (unchanged)    │
│                                                                      │
│  Agent::embed(text)                                                   │
│    ├─ embedding.provider == "mnn" → MnnInference::embed_text()       │
│    │                        · BGE-M3 sentence_embeddings output       │
│    │                        · same MRL-truncate + L2-normalize post   │
│    └─ otherwise          → existing /v1/embeddings HTTP path          │
│                                                                      │
│  MnnInference (singleton, backend/src/local/mnn_inference.cpp)        │
│    · lazy-loads MNN::Transformer::Llm      (Qwen3 chat model)         │
│    · lazy-loads MNN::Transformer::Embedding (BGE-M3)                  │
│    · thread-safe: one inference at a time, status queryable           │
└──────────────────────────────────────────────────────────────────────┘
```

### Threading model

MNN inference is **blocking and long-running** (seconds to minutes for LLM
generation). The engine's event loop is a single libuv thread with cooperative
fibers (`FiberNode`), so MNN must never run on that thread:

1. `Agent::call_llm` / `Agent::embed` detect `provider == "mnn"`.
2. They capture the current fiber, spawn a detached `std::thread` that runs
   the MNN call (including lazy model load on first use).
3. Token events are forwarded to the existing `on_event` callback from the
   worker thread (the C ABI already requires apps to marshal events to their
   UI thread; iOS/Android both do this).
4. On completion, the result is posted back to the owning fiber's loop via
   `FiberNode::spawn_back_on_loop` and the fiber is resumed — exactly the
   pattern the curl completion path uses.

Model **load** (first use or explicit `mc_local_load`) happens on the same
worker thread and is status-visible via `mc_local_status`.

### Model storage

Models are **not bundled in the app** (a 1.2 GB model exceeds practical app
download limits). They are downloaded by the app into the engine workspace:

```
<workspace>/models/
├── qwen3-1.7b/          # chat + summarization
│   ├── llm.mnn
│   ├── llm.mnn.weight
│   ├── llm_config.json
│   └── tokenizer.txt
└── bge-m3/              # embeddings
    ├── llm.mnn
    ├── llm.mnn.weight
    ├── embeddings_bf16.bin   # word-embedding table (~512 MB, required)
    ├── llm_config.json
    └── tokenizer.txt
```

On iOS the workspace is `Application Support/miniclaw`; on Android it is
`files/miniclaw`. The engine resolves the configured model directory relative
to the workspace. The app downloads files with progress UI (URLSession /
OkHttp) and the engine reads them from disk — no download code inside the
dylib, which keeps the C ABI small.

## Model catalog

### Qwen3-1.7B (chat / summarization) — "qwen 3 ~2G" class

Pre-converted by the MNN team, ready to download:

- Repo: [`taobao-mnn/Qwen3-1.7B-MNN`](https://huggingface.co/taobao-mnn/Qwen3-1.7B-MNN)
- Size: ~1.24 GB (int4 weights, `llm.mnn.weight`)
- Files: `llm.mnn`, `llm.mnn.weight`, `llm_config.json`, `tokenizer.txt`
- Default backend in the shipped config: Metal on Apple; override with
  `local.llm_backend` if needed.

Larger/smaller options (same layout, drop-in):

| Model | Repo | Weight size |
|---|---|---|
| Qwen3-0.6B | `taobao-mnn/Qwen3-0.6B-MNN` | ~0.4 GB |
| **Qwen3-1.7B (default)** | `taobao-mnn/Qwen3-1.7B-MNN` | ~1.2 GB |
| Qwen3-4B | `taobao-mnn/Qwen3-4B-MNN` | ~2.5 GB |
| Qwen3.5-0.8B / 2B / 4B / 9B | `taobao-mnn/Qwen3.5-*-MNN` | ~0.6 / 1.4 / 2.6 / 5 GB |

**Qwen3.5 note:** the official Qwen3.5-MNN exports are vision-language
models (`is_visual: true`, mrope, deepstack fusion). The runtime loads the
vision tower unconditionally, so **all six files** must be present:
`llm_config.json`, `tokenizer.txt`, `llm.mnn`, `llm.mnn.weight`,
`visual.mnn`, `visual.mnn.weight` — omitting the visual pair fails the load
with `Can't open file: .../visual.mnn`. Verified on MNN 3.6.1 (Windows,
Qwen3.5-2B-MNN and Qwen3.5-4B-MNN): both load and answer correctly
(`hello`, `Paris`, `2+2=4`, `17*68=1156`, one-sentence summarization);
embeddings stay tied in `llm.mnn.weight` via `tie_embeddings`, so no
separate `embeddings_bf16.bin` is needed.

**Qwen3.5-4B note (Windows):** the 4B weight file is ~2.6 GB, so its last
layers sit at file offsets ≥ 2 GiB. MNN 3.6.1's `FileLoader::offset()` used
plain `fseek()`, whose `off_t` is signed 32-bit on MinGW — every layer at an
offset ≥ 2 GiB failed to load (`ReadQuanData_c return weightLength is 0`) while
everything below 2 GiB worked. Fixed by using `_fseeki64` on MinGW as well:

```cpp
// source/core/FileLoader.cpp — FileLoader::offset()
#if defined(_MSC_VER) || defined(__MINGW32__)
    return _fseeki64(mFile, offset, SEEK_SET);
#else
    return fseek(mFile, offset, SEEK_SET);
#endif
```

Any model whose `llm.mnn.weight` exceeds ~2 GiB needs this patch on
Windows/MinGW builds (it is a no-op elsewhere; MSVC already used the 64-bit
form). The 2B model's weights are 1.2 GB, which is why it worked without it.

### BGE-M3 (embeddings)

BGE-M3 is a 568M-parameter multilingual text-embedding model (1024-dim,
MRL-truncatable). There is **no off-the-shelf MNN build**, so it must be
converted once. The stock MNN 3.6.1 exporter does **not** support BGE-M3's
XLM-RoBERTa architecture out of the box — a set of small, well-understood
patches is required (all verified end-to-end on Windows/MSYS2, MNN 3.6.1).

#### Conversion (one-time, dev machine)

Environment: native Windows Python 3.12 (venv) with `torch`,
`transformers>=5`, `onnx`, `numpy`, `sentencepiece`, and the PyPI `MNN`
package (provides the Python-side `mnnconvert`). MSYS2's Python cannot
install these wheels (platform tag mismatch), and the console must run in
UTF-8 mode (`python -X utf8`) or spinner output crashes on cp1252.

```bash
# 1. Clone MNN 3.6.1 and apply the exporter patches (see below)
git clone --depth 1 --branch 3.6.1 https://github.com/alibaba/MNN
cd MNN/transformers/llm/export

# 2. Export + convert (do NOT pass --transformerFuse: it crashes the MNN
#    optimizer on this graph; the non-fused model is functionally identical)
python -X utf8 llmexport.py \
    --path BAAI/bge-m3 \
    --type fp32 \
    --dst_path ~/miniclaw-models/bge-m3
```

This produces `~/miniclaw-models/bge-m3/{llm.mnn, llm.mnn.weight,
embeddings_bf16.bin, llm_config.json, tokenizer.txt}` (~1.8 GB: 1.2 GB
fp32 graph weights + 512 MB bf16 word-embedding table). Upload the folder to any
file host (or share it via AirDrop) and point the app at it, or place it
directly into `<workspace>/models/bge-m3/`.

#### Required patches

**Exporter — `transformers/llm/export/utils/model.py`** (XLM-RoBERTa support):

1. Add an `xlm-roberta` branch to `EmbeddingModel.forward()` (stock 3.6.1
   raises `Not support embedding model: xlm-roberta`). It mirrors the BERT
   path but uses `XLMRobertaModel` and bakes in mean-pooling + L2-norm
   (BGE-M3's official pooling) as the `sentence_embeddings` output.
2. In transformers 5.x, `XLMRobertaLayer.forward` returns a plain tensor, not
   a `(hidden, attn)` tuple — do **not** index `[0]` on it.
3. Feed `token_type_ids` (XLM-R's layer has the attribute; values are all 0)
   and **position ids offset by +2** (XLM-R position ids start at
   `padding_idx + 1`; the C++ engine feeds plain `arange(S)`).

**Exporter — `transformers/llm/export/utils/tokenizer.py`** (real spm scores):

Stock export writes all-zero piece scores, which makes MNN's runtime fall back
to greedy BPE merging. The patched export writes the real unigram log-probabilities
from the sentencepiece model (249 997 non-zero entries for BGE-M3) and
handles both `▁`-prefixed and space-form HF vocab keys.

**Runtime — `transformers/llm/engine/src/tokenizer/`** (MNN build patches):

1. **Viterbi unigram decoding** (`tokenizer.{cpp,hpp}`): MNN's stock spm
   encoder is a greedy BPE merge, which segmentates differently from
   sentencepiece/HF unigram Viterbi (e.g. `hello` → `hel`+`lo` instead of
   `hell`+`o`), producing embeddings that are only cos≈0.65 from the
   reference. The patch adds a true Viterbi decoder (first-byte buckets over
   NORMAL pieces, DP over positions) used when the vocab has non-zero scores;
   legacy all-zero files keep the old greedy path.
2. **NFKC compatibility normalization** (`nfkc_compat.hpp`, applied in
   `Sentencepiece::encode`): HF fast tokenizers and sentencepiece apply NFKC
   before spm pre-tokenization (fullwidth `，` → `,` etc.). Without it, CJK
   fullwidth punctuation tokenizes to UNK bytes. The patch ships a generated
   codepoint→replacement table (2 972 entries) and applies it before the `▁`
   normalization.

> **All runtime patches are applied automatically.** The MNN build is pulled
> via CMake FetchContent (pinned to 3.6.1), and `backend/CMakeLists.txt`
> applies the runtime patches at configure time by copying the fixed files
> from [`backend/third_party/mnn-patches/`](../backend/third_party/mnn-patches/)
> over the fetched source (plus a surgical `fseeki64` fix in
> `source/core/FileLoader.cpp`). Desktop, iOS and Android builds all get
> them; no manual re-copy is needed. If you upgrade the pinned MNN version,
> re-verify that the patch block still applies cleanly.

#### Verified embedding quality

With the converted model + patched runtime, MNN embeddings were compared
against the reference PyTorch BGE-M3 (mean-pool + L2-norm, same token
content) on five texts:

| Text | Cosine |
|---|---|
| `miniclaw local embedding quality test` | 0.999862 |
| `hello` | 0.999969 |
| `The weather in Shanghai is rainy today.` | 0.999902 |
| `天气真不错，适合出来散步。` (fullwidth punct) | 0.999986 |
| `Quarterly earnings beat expectations; …` | 0.999991 |

All **> 0.99**. The residual gap is bf16/fp32 word-embedding table + fp32
graph noise.

One known, accepted difference: MNN's C++ `Embedding` path feeds raw content
tokens and does **not** wrap them in BOS/SEP (HF tokenizers add `<s>`…`</s>`).
The wrapper shifts every vector by a constant amount (cos≈0.79 *between*
wrapped and unwrapped encodings of the same text), but since all app
embeddings — documents and queries alike — go through the same unwrapped
path, in-app retrieval quality is unaffected.

> The iOS/Android **Models** tab downloads the Qwen model straight from
> HuggingFace. For BGE-M3 it offers a "paste direct download links" flow:
> enter URLs for the four files (e.g. from your own HF repo) and the app
> fetches them into `models/bge-m3/`.

## Configuration

All keys live in `miniclaw.yaml` (editable in-app via Settings → Advanced, or
set programmatically with `mc_set_string`).

```yaml
# --- Summarization / memory distillation (local Qwen) ---
memory:
  provider: mnn                     # was: openai / any OpenAI-compatible name
  model: qwen3.5-4b                 # ignored for mnn; kept for parity
  endpoint: ~/.miniclaw/models/qwen3.5-4b   # ← the MNN model folder

# --- Embeddings (local BGE-M3) ---
embedding:
  provider: mnn
  model: bge-m3                     # ignored for mnn
  endpoint: ~/.miniclaw/models/bge-m3      # ← the MNN model folder
  dimension: 1024                   # BGE-M3 native dim; MRL truncation still applies

# --- Optional: main conversation local too ---
conversation:
  provider: mnn
  model: qwen3.5-4b
  endpoint: ~/.miniclaw/models/qwen3.5-4b  # same folder as memory (one shared slot)
```

Provider semantics:

- `provider: mnn` → route to `MnnInference`. The section's `endpoint:` field
  then doubles as the **local model folder** (absolute path, or relative to
  the workspace) — e.g. `memory.endpoint: ~/.miniclaw/models/qwen3.5-4b`.
  `model:` is ignored for mnn.
- Any other value → existing HTTP path, completely unchanged (`endpoint:` is
  the API URL as before).

Directory resolution order (first hit wins):

| Slot | 1st choice | Fallback |
|---|---|---|
| LLM (chat + summarization share one loaded model) | `conversation.endpoint` when `conversation.provider: mnn`, else `memory.endpoint` when `memory.provider: mnn` | `local.llm_model_dir` |
| Embedding | `embedding.endpoint` when `embedding.provider: mnn` | `local.embedding_model_dir` |

Endpoints starting with `http://`/`https://` are never treated as folders.
If both `conversation.endpoint` and `memory.endpoint` point at *different*
folders, the conversation one wins and a warning is logged (the LLM slot is a
single loaded model).

The folder must contain `llm_config.json` (+ weights, tokenizer) — MNN reads
`<folder>/llm_config.json` to discover the architecture; there is no model
registry, the directory *is* the model.

## C ABI additions (mobile)

```c
// JSON status of the local MNN runtime, e.g.:
// {"llm":{"loaded":false,"state":"idle","message":""},
//  "embedding":{"loaded":true,"state":"ready","message":"dim=1024"}}
char *mc_local_status(mc_engine *engine);            // caller frees with mc_free_string

// Preload a model in the background (kind = "llm" | "embedding").
// Resolves the model folder per the table above (endpoint-as-folder first,
// local.*_model_dir as fallback). Returns 0 if the load was started,
// -1 if already loading/loaded or invalid.
int  mc_local_load(mc_engine *engine, const char *kind);
```

## iOS / Android UI

Both apps have a **Models** tab (iOS: `LocalModelsView.swift` +
`ModelStore.swift`; Android: `ModelsTab.kt` + `ModelManager.kt`, built
programmatically over the same C ABI):

- One card per model (Qwen3-1.7B, BGE-M3) showing: approximate size, engine
  load status, download progress bar with cancel, and retry on failure.
  Files land in `<workspace>/models/<dirName>/`; already-downloaded files are
  skipped (no resume — a failed file restarts).
- BGE-M3 has no off-the-shelf MNN build, so its card shows the conversion
  steps plus an optional base-URL field for a self-hosted converted build.
- A **Load in engine** button calls `mc_local_load` and polls
  `mc_local_status` (2/5/10/20/40 s) until the slot reports loaded.
- Per-slot provider toggles — *Use on-device chat* (`conversation.provider`)
  and *Use on-device embeddings* (`embedding.provider`) — flip between
  `mnn` and `openai` via `mc_set_string`, persist across launches, and are
  re-applied when the engine starts. On-device distillation is configured
  separately: set `memory.distillation_provider: mnn` in the config editor.

## Performance notes

- Qwen3-1.7B int4 on an A15/A16-class phone: roughly 8–20 tokens/s on Metal
  (CPU-only is ~2–5 t/s). Daily-log distillation of a typical session
  (~2–4 KB) takes a few seconds to ~30 s.
- BGE-M3 fp16 embeds a paragraph in well under a second on CPU; the memory
  indexer batches documents, so re-indexing a workspace is fast.
- MNN uses mmap for `llm.mnn.weight`, so load time is dominated by model
  rearrange (a few seconds) and RAM usage stays bounded (~1.5–2 GB peak for
  Qwen3-1.7B — fine on 6 GB+ devices; use Qwen3-0.6B on older phones).

## Native desktop testing (Windows / macOS)

The full local-inference path can be exercised on a desktop before any
iOS/Android build, using the same C ABI the apps call:

```sh
cd backend
cmake -B build -G Ninja -DMC_USE_MNN=ON
cmake --build build --target mnn_local_test miniclaw
```

Test workspace (`/tmp/mnn-e2e/config.yaml`):

```yaml
server:
  port: 9123
conversation:
  provider: mnn
  model: qwen3-0.6b
memory:
  provider: mnn          # keeps distillation local too (no API key needed)
local:
  llm_model_dir: models/qwen3-0.6b
```

Download Qwen3-0.6B-MNN into `<workspace>/models/qwen3-0.6b/`:
`llm.mnn`, `llm.mnn.weight`, `llm_config.json`, `tokenizer.txt`
(from `taobao-mnn/Qwen3-0.6B-MNN` on HuggingFace).

```sh
# LLM: load + streamed local chat (prints MNN-LOCAL PASS on success)
build/mnn_local_test /tmp/mnn-e2e "What is 27*43? Reply with just the number."
# Embedding: load attempt (expects clean model-not-found until BGE-M3 is converted)
build/mnn_local_test /tmp/mnn-e2e ping embedding
# Full desktop server on the same C ABI
build/miniclaw /tmp/mnn-e2e
```

Model dirs can also be passed on the command line instead of (or on top of)
the workspace config — `--llm`/`--emb` merge into `<workspace>/config.yaml`
via the same `mc_set_string` path the app UI uses (bootstrapping it from
defaults if absent) and pin `conversation`/`memory` provider to `mnn`, so a
bare directory is enough:

```sh
build/mnn_local_test /tmp/mnn-e2e "hi" --llm /path/to/qwen3.5-4b
build/mnn_local_test /tmp/mnn-e2e ping embedding --emb /path/to/bge-m3
```

The values persist in `config.yaml` after the run (same as selecting a model
in the app).

Verified on Windows (MSYS2/ucrt64, MNN 3.6.1 CPU): engine create,
`mc_local_status`, `mc_local_load("llm")`, streamed local inference
(correct arithmetic answer), local memory distillation, and the full BGE-M3
embedding path all pass. Embedding quality against the reference PyTorch
model is verified in [Verified embedding quality](#verified-embedding-quality)
(cos > 0.9998 on five EN/ZH texts). A standalone diagnostic
(`backend/tests/emb_diag.cpp`, links `libminiclaw_core.dll.a`) dumps raw
MNN embeddings + token ids for direct comparison against HuggingFace.

### macOS (Mac Pro / any Apple desktop)

Same driver, native build. No extra flags needed: Apple (macOS + iOS)
always uses SQLite FTS5 for the memory index, so there is no Lucene++/Boost
dependency, and there is no PATH/DLL gotcha — the driver links
`libminiclaw_core.dylib` from the build tree automatically.

```sh
cd backend
cmake -B build-mac -DCMAKE_BUILD_TYPE=Release -DMC_USE_MNN=ON
cmake --build build-mac --target mnn_local_test -j "$(sysctl -n hw.ncpu)"
```

Models: the **BGE-M3 MNN files are a local conversion artifact** (see
[Conversion](#conversion-one-time-dev-machine)) — they are *not* on
HuggingFace, so copy them from the dev machine that ran the conversion:

```sh
mkdir -p ~/miniclaw-test/models
cd ~/miniclaw-test/models
scp -r user@dev-machine:/path/to/bge-m3 .            # ~2 GB (5 files)
# Qwen3.5 can be copied the same way or re-downloaded on the Mac:
#   huggingface-cli download taobao-mnn/Qwen3.5-4B-MNN --local-dir qwen3.5-4b
```

Run — pass the model dirs with `--llm`/`--emb`; no YAML editing needed
(the flags merge into `~/miniclaw-test/workspace/config.yaml`, creating it
from defaults if absent, and pin the provider to `mnn`):

```sh
mkdir -p ~/miniclaw-test/workspace

# LLM (expect MNN-LOCAL PASS; 17*68 → 1156)
backend/build-mac/mnn_local_test ~/miniclaw-test/workspace \
  "What is 17 * 68? Reply with only the number." \
  --llm $HOME/miniclaw-test/models/qwen3.5-4b
# Embedding (expect EMBED-LOAD PASS, dim=1024, l2_norm=1.0)
backend/build-mac/mnn_local_test ~/miniclaw-test/workspace ping embedding \
  --emb $HOME/miniclaw-test/models/bge-m3
```

On Apple platforms the repo's CMake forces `MNN_METAL=ON`, so the Mac Pro
build uses the Metal backend automatically (CPU fallback remains available
inside MNN if a layer is not Metal-supported).

### Windows PATH gotcha (0xC0000139)

If a freshly built exe dies with exit code `-1073741511`
(`STATUS_ENTRYPOINT_NOT_FOUND`) and no output, the loader is resolving
`libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libcurl-4.dll` from the wrong
MSYS2 environment. In MSYS2, `/mingw64/bin` often precedes
`/ucrt64/bin` in `PATH`; the backend is built against ucrt64, so run it with:

```sh
PATH="/c/msys64/ucrt64/bin:$PATH" build/mnn_local_test.exe ...
```

(`ldd build/mnn_local_test.exe` shows which environment each DLL resolved
from — all `lib*-*.dll` must come from the same one as the compiler.)

## Troubleshooting

| Symptom | Fix |
|---|---|
| `local model dir not found: models/qwen3-1.7b` | Download the model in the Models tab (or copy files into the workspace). |
| Exe exits with `-1073741511` and no output (Windows) | Wrong MSYS2 env on `PATH` — see "Windows PATH gotcha" above. |
| Summaries empty / instant "done" | Check Settings → provider is actually `mnn`; check device logs for `MnnInference` errors. |
| First message very slow | Model is loading on first use; tap "Load now" in Models tab to pre-warm. |
| Metal crashes on old iOS | Set `local.llm_backend: cpu`. |
| Embedding dimension mismatch after switching models | Delete the memory index (`<workspace>/memory_index/`) — it is rebuilt with the new model's dimensions. |
| `ReadQuanData_c return weightLength is 0` on Windows, only for large models | Weight file exceeds 2 GiB and MNN's `FileLoader::offset()` used 32-bit `fseek`. Fixed in-tree via the CMake patch block (`_fseeki64`); rebuild after pulling this change. |
| BGE-M3 embeddings drift (cos < 0.9) against reference | MNN runtime is missing the Viterbi/NFKC tokenizer patches — make sure the build used the `MC_USE_MNN` block from this repo (it applies them at configure time). |
