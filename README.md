# miniclaw 🦞🦀

`miniclaw` is a high-performance, lightweight personal AI agent system, initially forked from the `nanobot` project. It is designed for **simplified, cross-platform deployment** as a single application on Windows, Linux, macOS, and mobile devices, providing a transparent, fast, and local-first IDE-like experience for interacting with LLMs and local tools.

This project offers a high-performance C++ backend implementation designed for both a robust personal assistant and a platform for exploring advanced agentic architectures.

## 🚀 Key Features

- **ReAct Loop**: Intelligent reasoning and tool-use (Thought -> Action -> Observation).
- **Extensible Skill System**: Easily expand capabilities with modular, pluggable tools and knowledge.
- **Streaming SSE**: Real-time token streaming and tool event updates.
- **Async I/O**: 100% non-blocking networking and file operations.
- **3-Stage Distillation Memory**: Tiered memory management (Raw -> Daily -> Permanent) with event-based promotions (Periodic, Compaction, Session End).
- **Hybrid Search Engine**: Combined Vector Search (Faiss) and Keyword Search (Lucene++) with full disk persistence and reciprocal rank fusion.
- **Native Tools**:
    - `terminal`: Execute shell commands.
    - `file`: Robust reading and writing of local files.
    - `web`: Search (Brave) and fetch/summarize web pages.
- **Fiber/Coroutine Architecture**: Experimental stackful coroutines in C++ for lightning-fast responsiveness and smooth multitasking.

---

## 🏛 Backend Architecture 🦞
Located in `/backend`. This version is built for instant feedback and the exploration of lightweight, asynchronous agent logic.

- **Stack**: C++20, `libuv` (Event Loop), `libcurl` (Async IO), `libfiber` (Stackful Coroutines), `uWebSockets`.
- **Features**:
    - **Subagents**: Spawn background agent tasks that run in independent fibers.
    - **Fiber Scheduler**: Hybrid architecture that pulses `libuv` inside a fiber scheduler loop.
    - **Hybrid Memory System**: Built-in 3-stage distillation pipeline with RAG (Faiss) and BM25 (Lucene++) indexing.
    - **Session Persistence**: Full session management with local file storage.

**Run Backend:**
```bash
cd backend
./scripts/build.sh
./build/miniclaw
```

---

## 📦 Deployment Vision

`miniclaw` is engineered with the ultimate goal of simplified, cross-platform deployment. The intention is for users to install it as a self-contained application on various operating systems, including Windows, Linux, macOS, and mobile devices. This focus on "single-application" deployment aims to abstract away technical complexities, providing a seamless personal assistant experience accessible to a broad user base.

---

## 🛠 Prerequisites

- **C++**: CMake 3.20+, OpenSSL, libcurl.
- **API Keys**: Set `OPENAI_API_KEY` or configure your LLM/Embedding provider in `backend/config/config.yaml`.
- **Brave Search**: Optional `BRAVE_API_KEY` for web tools.

## 📖 Documentation

- [DESIGN_DOC.md](./DESIGN_DOC.md): Detailed architectural overview and implementation history.
- [backend/IDENTITY.md](./backend/IDENTITY.md): Project soul and core principles.

---
*miniclaw: a study in agentic performance.*
