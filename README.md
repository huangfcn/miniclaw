# miniclaw 🦞🦀

`miniclaw` is a high-performance, lightweight personal AI agent system, initially forked from the `nanobot` project. It is designed for **simplified, cross-platform deployment** as a single application on Windows, Linux, macOS, and mobile devices, providing a transparent, fast, and local-first IDE-like experience for interacting with LLMs and local tools.

This project offers two high-performance backend implementations to suit different needs and study purposes, serving as both a robust personal assistant and a platform for exploring advanced agentic architectures.

## 🚀 Key Features

- **ReAct Loop**: Intelligent reasoning and tool-use (Thought -> Action -> Observation).
- **Extensible Skill System**: Easily expand capabilities with modular, pluggable tools and knowledge.
- **Streaming SSE**: Real-time token streaming and tool event updates.
- **Async I/O**: 100% non-blocking networking and file operations.
- **Native Tools**:
    - `terminal`: Execute shell commands.
    - `file`: Robust reading and writing of local files.
    - `web`: Search (Brave) and fetch/summarize web pages.
- **Fiber/Coroutine Architecture**: Experimental stackful coroutines in C++ for maximum single-threaded efficiency.

---

## 🏛 Backend Implementations

### 1. C++ Backend (High-Performance Fibers) 🦞
Located in `/backend`. This version is built for extreme performance and exploration of low-level concurrency.

- **Stack**: C++20, `libuv` (Event Loop), `libcurl` (Async IO), `libfiber` (Stackful Coroutines), `uWebSockets`.
- **Unique Features**:
    - **Subagents**: Spawn background agent tasks that run in independent fibers.
    - **Fiber Scheduler**: Hybrid architecture that pulses `libuv` inside a fiber scheduler loop.
    - **Session Persistence**: Full session management with local file storage.

**Run C++ Backend:**
```bash
cd backend
./scripts/build.sh
./build/miniclaw
```

### 2. Rust Backend (Native Async) 🦀
Located in `/backend.rs`. A modern, clean, and safe implementation leveraging the best of the Rust ecosystem.

- **Stack**: Rust 2021, `Tokio` (Runtime), `Axum` (Web Framework), `Reqwest` (Networking).
- **Unique Features**:
    - **Single-file core**: The primary logic is contained in a highly readable `backend.rs` structure.
    - **Native Safety**: Leverages Rust's ownership model for thread-safe agent operations.

**Run Rust Backend:**
```bash
cd backend.rs
cargo run
```
*Note: Default port is 8081.*

---

## 📦 Deployment Vision

`miniclaw` is engineered with the ultimate goal of simplified, cross-platform deployment. The intention is for users to install it as a self-contained application on various operating systems, including Windows, Linux, macOS, and mobile devices. This focus on "single-application" deployment aims to abstract away technical complexities, providing a seamless personal assistant experience accessible to a broad user base.

---

## 🛠 Prerequisites

- **C++**: CMake 3.20+, OpenSSL, libcurl.
- **Rust**: Rustup / Cargo.
- **API Keys**: Set `OPENAI_API_KEY` (and optionally `BRAVE_API_KEY`) in your environment.

## 📖 Documentation

- [DESIGN_DOC.md](./DESIGN_DOC.md): Detailed architectural overview and implementation history.
- [backend/IDENTITY.md](./backend/IDENTITY.md): Project soul and core principles.

---
*miniclaw: a study in agentic performance.*
