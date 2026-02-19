# miniclaw: Design Document

## 1. Introduction

`miniclaw` is a lightweight, personal AI Agent system designed for performance, transparency, and local control. It serves as a study in building efficient, asynchronous agentic systems.

The project has evolved from a Python-based exploration into two distinct, high-performance backend implementations:
1.  **C++ Backend**: An experimental architecture using stackful coroutines (fibers) integrated with an asynchronous event loop (`libuv`).
2.  **Rust Backend**: A modern, production-ready implementation leveraging the `Tokio` runtime and `Axum`.

---

## 2. Architecture Overview

### 2.1. C++ Backend Architecture (The "Fiber" Model) 🦞

The C++ backend is designed to handle thousands of concurrent agent operations within a single OS thread using **stackful coroutines**.

- **Concurrency Model**: It utilizes a customized `libfiber` implementation. Unlike OS threads, fibers are light on resources and allow for context switching with minimal overhead.
- **Async Bridge (libuv)**: To prevent blocking the fiber scheduler, a bridge was built between the fiber library and `libuv`. The fiber scheduler "pulses" the `libuv` event loop, allowing non-blocking I/O to wake up suspended fibers.
- **Networking**: `libcurl` (multi-interface) is used for all LLM and Web requests. When a request is initiated, the current fiber is suspended via `fiber_suspend()`. Once `libcurl` signals completion through `libuv` polling, the fiber is resumed.
- **Subagents**: Leveraging fibers, `miniclaw` can spawn background agents that execute complex ReAct loops independently of the main user interaction.

### 2.2. Rust Backend Architecture (Native Async) 🦀

The Rust backend provides a safe and idiomatic implementation of the same ReAct logic.

- **Runtime**: Powered by `Tokio`, the industry standard for asynchronous programming in Rust.
- **Web Framework**: Uses `Axum` for handling HTTP requests and SSE streaming.
- **Tools**: Re-implemented in native Rust, utilizing traits to define a common interface for `terminal`, `file`, and `web` tools.
- **Simplicity**: Targeted at being a highly readable and maintainable reference implementation.

---

## 3. Core Components

### 3.1. ReAct Loop
Both backends implement a "Thought-Action-Observation" loop. 
- **Parsing**: A regex-based parser extracts tool calls in the format `<tool name="...">...</tool>`.
- **Recursion**: Tool results are fed back into the message history as `<observation>` blocks, triggering further "Thought" until a final answer is produced.

### 3.2. Tools System
Tools are implemented as independent modules:
- **Terminal**: Direct shell access (platform dependent).
- **Filesystem**: Sandboxed file access for project management.
- **Web Interface**: Integrates with Brave Search for search and `reqwest`/`libcurl` for page fetching, with automated HTML stripping for context efficiency.

---

## 4. Technical Evolution

The project started as an analysis of `nanobot` (Python) and `picoclaw` (Go). While Python offers the richest ecosystem, C++ and Rust were chosen for `miniclaw` to explore:
- **Single-binary distribution**: Avoiding complex runtime dependencies (Python/Node.js).
- **Low-level Concurrency**: Solving the C10K problem for AI agents using fibers vs. native async.
- **Performance**: Minimizing latency in token streaming and tool execution.

## 5. Deployment & User Experience

`miniclaw` emphasizes an IDE-style layout in its (forthcoming) Next.js frontend, where the Agent's internal thoughts and tool actions are as prominent as the final response. This transparency is a core design principle.

---
*Last Updated: February 2026*