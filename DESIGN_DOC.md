# miniclaw: Design Document

## 1. Introduction

miniclaw is envisioned as a lightweight, transparent, and personal AI Agent system, reimplemented to optimize the core experience of OpenClaw for local, personal use. A primary goal is to provide a seamless "Windows first" deployment and user experience through a single, native executable, leveraging Rust for its performance and safety benefits. The project emphasizes file-first memory, skills as plugins, and complete transparency of Agent operations.

## 2. Original miniclaw Design (Python/FastAPI)

Based on `docs/analysis_01.txt` and `docs/miniclaw.README.txt`, the initial design proposed a Python-centric stack:

*   **Backend Language:** Python 3.10+ (with Type Hinting).
*   **Web Framework:** FastAPI (RESTful API, SSE).
*   **Agent Orchestration:** LangChain 1.x (using `create_agent`).
*   **RAG Engine:** LangChain 1.0+ with Faiss/Chroma for vector storage.
*   **LLM Interface:** OpenAI API format compatible.
*   **Data Storage:** Local file system (Markdown/JSON).
*   **Core Tools:** Terminal, Python REPL, Fetch URL (wrapped), Read File, RAG Retrieval.
*   **Skills System:** Instruction-following Markdown files (`SKILL.md`).
*   **Memory Management:** File-based (`MEMORY.md`, JSON sessions), system prompt composition from Markdown files.
*   **Frontend:** Next.js 14+ (App Router), TypeScript, Shadcn/UI, Monaco Editor, IDE-style layout.

## 3. Reference Design Analysis

To inform the "Windows first" deployment strategy and overall architecture, two reference projects were investigated: `nanobot` and `picoclaw`.

### 3.1. Nanobot (Python/Node.js)

`nanobot` is an "ultra-lightweight" Python-based personal AI assistant.

*   **Key Architectural Patterns:**
    *   Python backend for core agent logic (AgentLoop, ContextBuilder, MemoryStore, SkillsLoader).
    *   Asynchronous `MessageBus` decouples channels from the agent.
    *   Plugin-based `ChannelManager` for multi-platform chat integrations.
    *   `LiteLLMProvider` for flexible multi-provider LLM access.
    *   Robust `ExecTool` with safety guards and path traversal protection.
    *   File-based memory system (`MEMORY.md`, `HISTORY.md`).
    *   Markdown-based instruction-following skills (`SKILL.md`).
*   **Strengths:** Rich Python AI/ML ecosystem, flexible LLM integration, comprehensive channel support.
*   **Weaknesses (for "Windows first" native deployment):**
    *   Python environment setup can be complex for end-users without developer tools.
    *   The **Node.js dependency for WhatsApp integration** adds a separate runtime requirement, complicating a single-installer experience.
    *   Some skills (e.g., `tmux`) explicitly recommend WSL on Windows, indicating reliance on Linux-centric tools.
*   **Lessons Learned:** Python's ecosystem is powerful, but managing multiple runtimes (Python + Node.js) for native Windows deployment adds complexity that often pushes towards containerization (Docker/WSL).

### 3.2. PicoClaw (Go)

`picoclaw` is an ultra-efficient Go reimplementation of `nanobot`'s concepts.

*   **Key Architectural Patterns:**
    *   **Single, cross-platform executable:** The entire application is compiled into one Go binary.
    *   Go-native implementations for agent core, tools, memory, and most channel integrations.
    *   Extreme efficiency with low resource usage and fast boot times.
    *   Platform-aware `ExecTool` (adapts to PowerShell on Windows).
    *   Hardware interaction tools (`i2c`, `spi`) for embedded Linux.
*   **Strengths:**
    *   **Optimal for "Windows first" native deployment:** Go's static compilation significantly simplifies distribution as a single `.exe` file.
    *   Excellent performance and resource efficiency.
    *   Highly portable across various architectures.
*   **Weaknesses (for "Windows first" native deployment):**
    *   Its **WhatsApp channel still relies on an external Node.js bridge**, which means a separate runtime dependency if WhatsApp is desired.
    *   The AI/ML ecosystem in Go is less mature than Python's, but `picoclaw` effectively abstracts LLM interactions via HTTP calls.
*   **Lessons Learned:** A compiled language like Go (or Rust) is ideal for "Windows first" native deployment due to single-binary distribution. However, external dependencies for specific channels (like WhatsApp) can still introduce multi-runtime requirements.

### 3.3. Comparison & Impact on miniclaw

The analysis revealed that while Python excels in AI/ML ecosystem richness, compiled languages are inherently superior for producing single, easy-to-distribute native executables on Windows. The `picoclaw` project serves as a compelling proof-of-concept for this approach in an AI agent context. The persistent external Node.js dependency for WhatsApp integration across both projects highlights a common challenge for that specific channel.

## 4. Revised miniclaw Implementation Plan (C++ Backend + Next.js Frontend)

To achieve the "Windows first" deployment goal while maintaining commercial-grade performance, miniclaw will now adopt a **C++ backend**. This leverages C++'s universality, performance, and ability to produce a single, native executable with minimal dependencies.

### 4.1. Core Principles

*   **Native C++ Backend:** Leverage Modern C++ (C++17/20) for maximum performance and portability.
*   **File-First Memory:** All memory, skills, and configuration are transparently stored in local Markdown/JSON files.
*   **Modular Architecture:** Clear separation between the C++ backend (API) and the Next.js web frontend.
*   **Header-only Preference:** Favor header-only libraries (e.g., Crow, nlohmann/json) to simplify build and deployment.

### 4.2. Backend Design (C++)

1.  **Language & Ecosystem**: C++20, using CMake for build management.
2.  **Web Framework**: `CrowCpp` (Crow).
    *   Fast, easy-to-use, Flask-like microframework.
    *   Built-in WebSocket and HTTP support.
    *   Header-only, easing integration.
3.  **Project Structure**:
    *   `CMakeLists.txt`: Build configuration.
    *   `src/`: Source code (`main.cpp`, `agent/`, `tools/`).
    *   `include/`: Header files.
    *   `third_party/`: Dependencies (fetched via CMake or submodules).
4.  **Agent Core**:
    *   Implement `Agent` class in C++.
    *   Manage message history and LLM interaction loop.
5.  **Core Tools**: Implement tools as C++ classes inheriting from a `Tool` interface.
    *   **Terminal**: Use `reproc` or `std::system` (with strict parsing) for command execution.
    *   **File I/O**: Use `std::filesystem`.
    *   **Web**: Use `cpr` (C++ Requests) for HTTP requests.
    *   **Python**: Execute Python scripts via `reproc` or `popen`.
6.  **Memory**:
    *   Use `nlohmann/json` for structured session storage.
    *   Use `hnswlib` (header-only) for local vector search.
    *   Use `cpr` to call OpenAI/compatible APIs for embeddings.

### 4.3. Frontend Design (Next.js Web Frontend)

1.  **Framework**: Next.js 14+ (App Router), TypeScript.
2.  **Communication**: REST API + Server-Sent Events (SSE) from the C++ backend.
3.  **Deployment**: Compiled to static assets and served by Crow.

## 5. Detailed API Design (C++/Crow)

The backend will expose a RESTful API.

### 5.1. Chat & Agent
*   **`POST /api/chat`**
    *   **Body**: `{"message": "string", "session_id": "string", "stream": true}`
    *   **Response**: SSE stream of tokens and tool events.

### 5.2. Sessions
*   **`GET /api/sessions`**: List sessions.
*   **`GET /api/sessions/:id`**: Get session history.
*   **`POST /api/sessions`**: Create new session.

### 5.3. Files & Skills
*   **`GET /api/files`**: Read file (sandboxed).
*   **`POST /api/files`**: Write file.
*   **`GET /api/skills`**: List available skills.

## 6. C++ Data Structures

```cpp
// Core Structures

struct Message {
    std::string role;
    std::string content;
};

struct Session {
    std::string id;
    std::string title;
    std::vector<Message> messages;
    int64_t updated_at;
};

struct AgentEvent {
    enum class Type { Token, ToolStart, ToolEnd, Error, Done };
    Type type;
    std::string content;
    // ...
};
```

## 7. Agent Logic (C++)

1.  **Load System Prompt**: Read Markdown files from disk.
2.  **LLM Request**: Construct JSON body for OpenAI API.
3.  **Network Call**: Use `cpr::Post` with streaming callback.
4.  **Stream Parsing**: Parse tokens. If specific XML/Tool pattern is detected:
    *   Buffer tool arguments.
    *   Execute C++ tool function.
    *   Append result to history.
    *   Recurse (send history + result back to LLM).