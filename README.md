# miniclaw 🦞🦀

`miniclaw` is a high-performance, lightweight personal AI agent system. It is designed for **simplified, cross-platform deployment** as a single application on Windows, Linux, macOS, and mobile devices, providing a transparent, fast, and local-first IDE-like experience for interacting with LLMs and local tools.

This project features a lightning-fast **C++ Backend** powered by stackful coroutines (fibers) and a modern **Tauri-based Desktop GUI** for a premium user experience.

---

## 🚀 Key Features

- **ReAct Loop**: Intelligent reasoning and tool-use (Thought -> Action -> Observation).
- **Extensible Skill System**: Modular, pluggable tools for terminal access, file management, and web browsing.
- **Hybrid Search Engine**: Combined Vector Search (Faiss) and Keyword Search (Lucene++) for deep memory.
- **3-Stage Distillation Memory**: Tiered memory management (Raw -> Daily -> Permanent).
- **Fiber/Coroutine Architecture**: Experimental stackful coroutines in C++ for extreme concurrency.
- **Fluent Chat UX**: A clean, responsive interface for interacting with your personal AI.
- **Real-time Monitoring**: Visual telemetry for engine status, fiber nodes, and system logs.
- **Visual Configuration**: Edit `config.yaml` directly within the app with instant reloading.

---

## 🏛 Architecture

The system is built on a three-layer stack:

1.  **Core Engine (C++)**: The "brain" of miniclaw. Built with C++20, `libuv` (Event Loop), `libcurl` (Async IO), and `libfiber` (Stackful Coroutines).
2.  **App Layer (Tauri + Rust)**: Manages the lifecycle of the core engine, handles system-level permissions, and provides a cross-platform bridge.
3.  **Frontend (React + Vite + Tailwind)**: A modern, high-performance web interface.

---

## 🛠 Getting Started

### Prerequisites

- **C++**: CMake 3.20+, OpenSSL, libcurl.
- **Rust**: Latest stable toolchain (for Tauri).
- **Node.js**: LTS version (for Frontend).
- **API Keys**: Set `OPENAI_API_KEY` or configure your provider in the app.

### Installation & Setup

#### 1. Backend Setup
First, prepare the C++ core:
```bash
cd backend
./tools/setup_deps.sh
./tools/build.sh
```

#### 2. Frontend Setup (First Time)
Install the web dependencies:
```bash
cd frontend
npm install
```

### Running the Application

#### 1. Running Components Separately
For development and independent testing, you can start the engine and the GUI in two steps:

**Start the Backend Engine:**
```bash
cd backend
./build/miniclaw
```

**Start the Frontend GUI:**
```bash
cd frontend
npm run tauri dev
```

#### 2. Running via Tauri
This launches the GUI and automatically starts the C++ core as a sidecar:
```bash
cd frontend
npm run tauri dev
```

---

## 🖥 User Interface

`miniclaw` offers a multi-faceted interface designed for both performance and transparency:

- **Chat Interface**: The primary interaction layer with full history management and real-time streaming tokens.
- **Monitoring Tab**: Launch and shut down the core engine, view real-time logs, and track system metrics like active fibers and uptime.
- **Settings Tab**: Directly modify the engine configuration, switch LLM providers, and manage model endpoints without leaving the app.

---

## 📦 Deployment Vision

`miniclaw` is engineered with the goal of being a **single-binary installation**. The Tauri framework allows us to bundle the high-performance C++ backend as a sidecar, providing a seamless "click-to-run" experience across all major desktop and mobile platforms.

---

## 📖 Documentation

- [DESIGN_DOC.md](./DESIGN_DOC.md): Detailed architectural overview and implementation history.
- [backend/IDENTITY.md](./backend/IDENTITY.md): Project soul and core principles.

---
*miniclaw: a study in agentic performance.*
