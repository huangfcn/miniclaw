# miniclaw: Design Document

## 1. Introduction

`miniclaw` is a lightweight, personal AI Agent system designed for performance, transparency, and local control. It serves as a study in building efficient, asynchronous agentic systems.

The project utilizes an experimental architecture using stackful coroutines (fibers) integrated with an asynchronous event loop (`libuv`).

---

## 2. Architecture Overview

### 2.1. Backend Architecture (The "Fiber" Model) 🦞

The C++ backend is designed for **lightning-fast responsiveness** and smooth multitasking, allowing the agent to execute complex background tasks without pausing the main interaction loop.

- **Concurrency Model**: It utilizes a customized `libfiber` implementation. Unlike OS threads, fibers are light on resources and allow for context switching with minimal overhead.
- **Async Bridge (libuv)**: To prevent blocking the fiber scheduler, a bridge was built between the fiber library and `libuv`. The fiber scheduler "pulses" the `libuv` event loop, allowing non-blocking I/O to wake up suspended fibers.
- **Networking**: `libcurl` (multi-interface) is used for all LLM and Web requests. When a request is initiated, the current fiber is suspended via `fiber_suspend()`. Once `libcurl` signals completion through `libuv` polling, the fiber is resumed.
- **Subagents**: Leveraging fibers, `miniclaw` can spawn background agents that execute complex ReAct loops independently of the main user interaction.

---

## 3. Core Components

### 3.1. ReAct Loop
The backend implements a "Thought-Action-Observation" loop. 
- **Parsing**: A regex-based parser extracts tool calls in the format `<tool name="...">...</tool>`.
- **Recursion**: Tool results are fed back into the message history as `<observation>` blocks, triggering further "Thought" until a final answer is produced.

### 3.2. Tools System
Tools are implemented as independent modules:
- **Terminal**: Direct shell access (platform dependent).
- **Filesystem**: Sandboxed file access for project management.
- **Web Interface**: Integrates with Brave Search for search and `reqwest`/`libcurl` for page fetching, with automated HTML stripping for context efficiency.

### 3.3. Memory System (3-Stage Distillation) 🧠
`miniclaw` implements a tiered memory system to handle long-running agent contexts efficiently:
- **Layer 1 (Raw Logs)**: Raw session history stored in `sessions/*.jsonl`.
- **Layer 2 (Daily Summaries)**: LLM-summarized daily logs in `memory/YYYY-MM-DD.md`, triggered during context compaction.
- **Layer 3 (Permanent Facts)**: Curated project conventions and permanent facts in `MEMORY.md`.

**Hybrid Search Engine**:
The memory system utilizes a high-performance C++ hybrid search engine:
- **Vector Search**: Faiss (`IndexFlatIP`) for semantic retrieval. The index is persisted to disk (`faiss.index`) and loaded into RAM at startup for high efficiency.
- **Keyword Search**: Lucene++ for traditional BM25 keyword matching, also persisted to disk.
- **Ranking**: A fused ranking mechanism (reciprocal rank fusion) combines semantic and keyword results, with temporal decay applied to daily logs.
- **Configurable Routing**: Unified `provider`, `model`, and `endpoint` configuration for the interaction LLM (`conversation`), memory LLM (`memory`), and `embedding` model.

---

## 4. Technical Evolution

The project started as an analysis of `nanobot` (Python) and `picoclaw` (Go). While Python offers the richest ecosystem, C++ was chosen for `miniclaw` to explore:
- **Single-binary distribution**: Avoiding complex runtime dependencies (Python/Node.js).
- **Low-level Concurrency**: Solving the C10K problem for AI agents using fibers vs. native async.
- **Performance**: Minimizing latency in token streaming and tool execution.

## 5. Deployment & User Experience

`miniclaw` emphasizes an IDE-style layout in its (forthcoming) Next.js frontend, where the Agent's internal thoughts and tool actions are as prominent as the final response. This transparency is a core design principle.

---
*Last Updated: February 2026 (Unified Configuration & Memory Persistence Update)*