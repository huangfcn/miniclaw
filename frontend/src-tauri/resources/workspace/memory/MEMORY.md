# Long-Term Memory (Curation)

## User Context
**Region:** New York, New York, USA
**Assistant:** miniclaw

## Technical Constraints
### C++ Backend (Miniclaw)
- **Framework:** C++20 Project with FAISS/Lucene++ hybrid search
- **Build System:** CMake 3.20+ with Ninja generator, libuv-based fiber coroutines
- **Libraries:** libcurl, uWebSockets
- **Blockers:** Compilation error in `src/tools/busybox.hpp` (`'Config' has no member named 'tools_path'`)
- **API/Keys:** `BRAVE_API_KEY` currently unset for web tools (affects non-weather fetch utilities)

### Weather Stack
- **Primary:** Open-Meteo (Preferred for reliability/no API key)
- **Fallback:** wttr.in (No API key required)
- **Status:** High-frequency monitoring suspended pending environment stability.

### Environment
- **Utility Preference:** Python preferred for Bash drop-in replacement (BusyBox-style)

## Core Preferences
- **Communication:** Concise answers (per SOUL.md guidelines)
- **Focus:** Chemistry Education (Nuclear Transmutation), Geometry, C++ Development
- **Workflows:** Multi-platform support (Windows, macOS, Linux)
- **Stability:** Priority on environment configuration (Gmail auth, API keys)

## Current Status
- **Project:** Miniclaw C++ Backend Development
- **Pending:** Resolve `Config` singleton `tools_path` method or update initialization logic
- **Issues:** Gmail auth errors (credentials not configured), Web fetch failures without API keys
- **Action Plan:** Remediate build environment and API key configuration to resume weather queries and testing.