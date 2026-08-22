#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Mobile path sandboxing
//
// On mobile builds (MC_MOBILE defined) the agent runs inside the app sandbox.
// File tools must not be able to read or write outside the workspace
// directory (the app's private data dir). This helper enforces that.
//
// Desktop builds: no-op, all paths allowed.
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <filesystem>
#include <string>

#ifdef MC_MOBILE

namespace mobile_sandbox {

// Returns an error message if `path` escapes `workspace`, empty string if OK.
inline std::string check_path(const std::string &path,
                              const std::string &workspace) {
  namespace fs = std::filesystem;
  try {
    fs::path root = fs::canonical(workspace);
    fs::path p = fs::path(path);
    if (p.is_relative())
      p = root / p;
    // Lexical normalization first (target may not exist yet for writes).
    p = p.lexically_normal();
    if (!std::equal(root.begin(), root.end(), p.begin(), p.end())) {
      return "Error: path is outside the app sandbox";
    }
  } catch (const std::exception &) {
    // canonical() fails if the workspace doesn't exist yet; fall back to a
    // lexical check against the raw workspace string.
    namespace fs = std::filesystem;
    fs::path root = fs::path(workspace).lexically_normal();
    fs::path p = fs::path(path);
    if (p.is_relative())
      p = root / p;
    p = p.lexically_normal();
    if (!std::equal(root.begin(), root.end(), p.begin(), p.end())) {
      return "Error: path is outside the app sandbox";
    }
  }
  return "";
}

} // namespace mobile_sandbox

// var = error string (empty when OK); returns the error from the tool if the
// path escapes the sandbox. `path_expr` and `ws_expr` are evaluated once.
#define MC_MOBILE_PATH_CHECK(var, path_expr, ws_expr)                \
  do {                                                               \
    std::string var = mobile_sandbox::check_path(path_expr, ws_expr); \
    if (!var.empty()) return var;                                    \
  } while (0)

#else

#define MC_MOBILE_PATH_CHECK(var, path_expr, ws_expr) \
  do {                                                \
  } while (0)

#endif
