# OpenClaw Memory Architecture

OpenClaw employs a multi-tiered memory system designed to manage long-running agent contexts efficiently while staying within LLM token limits. The system prevents a "goldfish memory" effect by periodically distilling raw chat history into compact, durable markdown files, relying heavily on the LLM's own context management capabilities.

The memory lifecycle in OpenClaw transitions data through a three-stage distillation pipeline: `sessions/{id}.jsonl` -> `memory/{date}.md` -> `MEMORY.md`.

## The Three Layers of Memory

### 1. Raw Session Logs: `sessions/{session_id}.jsonl` (Layer 1)
This is the ephemeral entry point. OpenClaw automatically persists every conversation to disk as an append-only event log.
- **Content:** Raw conversation history (JSON Lines), message timestamps, session status, and tool call traces.
- **Purpose:** Acts as the primary sliding-window record for current session context.
- **Indexing:** This raw history is indexed by OpenClaw's local SQLite database using both Vector Search (Embeddings) and Keyword Search (BM25 FTS5). However, as the conversation grows, the active context loaded to the LLM becomes too large, hitting token thresholds.

### 2. Daily Logs: `memory/YYYY-MM-DD.md` (Layer 2)
The intermediate layer where the agent "compresses" the noisy session data into a daily journal format.
- **Content:** An append-only running log of what happened throughout a specific day, including decisions made, activities, modified files, and technical constraints.
- **Automation (Context Compaction & Memory Flush):**
  - OpenClaw actively tracks the token count of a running session. When the context exceeds a soft threshold (e.g., 40k tokens), it triggers a background routine.
  - Before truncating the session history to recover token space, a "Pre-compaction memory flush" is automatically invoked.
  - The LLM receives an internal instruction (e.g., *"Pre-compaction memory flush turn. The session is near auto-compaction; capture durable memories to disk."*).
  - The LLM summarizes the recent activity and autonomously uses standard file tools to **append** the summary to `memory/YYYY-MM-DD.md`.
- **Indexing:** Like the session logs, the Daily Logs are aggressively indexed into SQLite (BM25 + RAG). Because these files contain "distilled" summaries, they yield much higher-quality semantic search results than the raw `.jsonl` files. OpenClaw typically loads today’s and yesterday’s logs into the active context at session startup.

### 3. Curated Memory: `MEMORY.md` (Layer 3)
The final destination for long-term, high-value, and permanent information.
- **Content:** A curated, stable file containing permanent facts, project conventions, technical stacks, user preferences (e.g., "User prefers Python over Java"), and critical directives.
- **Management:** Unlike the append-only daily logs, `MEMORY.md` is strictly edited in-place by the agent using file-editing tools (like `edit_file` or `replace_block`) to maintain a clean, duplicate-free state.
- **Loading:** Rather than relying purely on search/RAG indexing, `MEMORY.md` is often injected *directly* into the System Prompt or active context. The agent essentially has this file permanently "taped to its monitor" to ensure it never violates a core rule or forgets a fundamental user preference.

## Workflow & Indexing Summary

| Layer | File / Path | Format | Retention | Search / Injection Strategy |
|---|---|---|---|---|
| **L1 (Raw)** | `sessions/*.jsonl` | JSONL | Temporary/Full Archive | Searchable via Hybrid Search (RAG/BM25). Active context is *truncated* automatically when token limits are hit. |
| **L2 (Mid-term)** | `memory/*.md` | Markdown | Daily Summaries | Searchable via Hybrid Search. Recent files (today/yesterday) are directly loaded as context. |
| **L3 (Permanent)** | `MEMORY.md` | Markdown | Curated Core Facts | Continually edited via tool calls; usually injected directly into the LLM system prompt. |
| **L0 (Identity)** | `SOUL.md` / `USER.md` | Markdown | Persona & Identity | Directly loaded system prompts defining agent constraints and user profile. |

## How the L1 to L2 Compaction Operates
When a conversation gets too large:
1. **The Threshold Hit:** The Active Context (in-memory) reaches the token limit. (The physical `.jsonl` file keeps growing unhindered for archival).
2. **The Flush Prompt:** The system sends an internal instruction to the LLM to write out durable memories to `memory/YYYY-MM-DD.md`.
3. **The Compaction:** Older messages are removed from the Active Context (the API payload) and replaced by a single "Summary Message" at the top of the context window.
4. **The Reset:** The token count drops significantly, freeing up space for the conversation to continue.

## Promoting to L3 (MEMORY.md)
If an extracted fact is heavily entrenched or represents a persistent system rule, the LLM decides to update `MEMORY.md`. 
Instead of blindly appending to the file, the agent uses its "Read-Modify-Write" cycle:
- It uses a Markdown Header (e.g., `## Tech Stack`) to locate the relevant section.
- It overwrites outdated facts (e.g., changing Python 3.10 to Python 3.12).
- It applies the file patch, cleanly cementing the memory into the L3 layer.