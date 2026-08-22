#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Mobile shell: pure-C++ implementations of common Unix commands
//
// Android forbids fork() in app processes, so popen()/system() cannot be
// used. Instead the `exec` tool on mobile parses the command line and
// dispatches to in-process C++ functions — no child process is ever created.
//
// Supported: ls cat head tail wc grep find mkdir rm cp mv touch pwd date
//            echo sort uniq cut basename dirname which true
// Pipelines (cmd | cmd) are supported via in-memory strings. No other shell
// features (redirection, &&, variables, subshells) — the tool tells the
// model this explicitly so it composes with dedicated tools instead.
//
// All file access is confined to the app workspace (mobile_sandbox).
// Only compiled into mobile builds (MC_MOBILE).
// ─────────────────────────────────────────────────────────────────────────────

#ifdef MC_MOBILE

#include "tool.hpp"
#include "mobile_sandbox.hpp"
#include "../config.hpp"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
// NOTE: no <fnmatch.h> — it is POSIX-only (present in Android bionic but not
// every toolchain). We use a small self-contained glob matcher instead so the
// same code compiles on Android, iOS, and desktop.
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <vector>

namespace mshell {

namespace fs = std::filesystem;
constexpr size_t kMaxOutput = 10000; // chars, matches desktop exec truncation

// ── helpers ────────────────────────────────────────────────────────────────

inline std::string workspace() { return Config::instance().memory_workspace(); }

// True if `p` is `root` itself or located underneath it (element-wise).
inline bool is_within(const fs::path &root, const fs::path &p) {
  auto r = root.begin(), e = root.end();
  auto q = p.begin();
  for (; r != e; ++r, ++q) {
    if (q == p.end() || *q != *r)
      return false;
  }
  return true; // consumed all of root; p is root or longer
}

// Resolve `path` against the workspace and verify it stays inside the
// sandbox. Returns "" on success (out = absolute path) or an error string.
inline std::string resolve(const std::string &path, fs::path &out) {
  if (path.empty())
    return "Error: empty path";
  try {
    fs::path root = fs::canonical(workspace());
    fs::path p = fs::path(path);
    if (p.is_relative())
      p = root / p;
    p = p.lexically_normal();
    if (!is_within(root, p))
      return "Error: path is outside the app sandbox";
    out = p;
  } catch (const std::exception &) {
    // Workspace may not exist yet — fall back to a lexical check.
    fs::path root = fs::path(workspace()).lexically_normal();
    fs::path p = fs::path(path);
    if (p.is_relative())
      p = root / p;
    p = p.lexically_normal();
    if (!is_within(root, p))
      return "Error: path is outside the app sandbox";
    out = p;
  }
  return "";
}

// Read a whole file. Returns "" on success (out = content) or an error.
inline std::string read_all(const fs::path &p, std::string &out) {
  std::ifstream f(p, std::ios::binary);
  if (!f)
    return "Error: cannot open " + p.string();
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return "";
}

// Split into lines without trailing newlines.
inline std::vector<std::string> split_lines(const std::string &s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r')
        cur.pop_back();
      lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  // Only push a final line if there is content after the last newline.
  if (!cur.empty()) {
    lines.push_back(cur);
  }
  return lines;
}

// Convert a shell glob (supports '*', '?', '[...]') to an anchored regex.
static std::string glob_to_regex(const std::string &g) {
  std::string re = "^";
  for (size_t i = 0; i < g.size(); ++i) {
    char c = g[i];
    if (c == '*')
      re += ".*";
    else if (c == '?')
      re += ".";
    else if (c == '[') {
      // Copy the whole character class verbatim (regex uses the same syntax).
      re += '[';
      ++i;
      if (i < g.size() && (g[i] == '!' || g[i] == '^')) {
        re += '^';
        ++i;
      }
      while (i < g.size() && g[i] != ']') {
        re += g[i];
        ++i;
      }
      if (i < g.size())
        re += ']'; // the closing bracket
    } else if (std::string(".\\+(){}|^$ ").find(c) != std::string::npos) {
      re += '\\';
      re += c;
    } else {
      re += c;
    }
  }
  re += "$";
  return re;
}

// True if `name` matches the glob `pattern`.
inline bool glob_match(const std::string &pattern, const std::string &name) {
  try {
    return std::regex_match(name, std::regex(glob_to_regex(pattern)));
  } catch (const std::exception &) {
    return false;
  }
}

inline std::string truncate(const std::string &s) {
  if (s.size() <= kMaxOutput)
    return s;
  return s.substr(0, kMaxOutput) + "\n... [truncated at 10000 chars]";
}

// Parse "-n 5" / "-n5" style numeric option. Returns false if not present.
inline bool opt_int(const std::vector<std::string> &args, const std::string &flag,
                    int &value) {
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &a = args[i];
    if (a == flag && i + 1 < args.size()) {
      value = std::atoi(args[i + 1].c_str());
      return true;
    }
    if (a.size() > flag.size() && a.compare(0, flag.size(), flag) == 0 &&
        flag.size() == 2 && a[1] == flag[1]) {
      // -n5 form
      value = std::atoi(a.c_str() + 2);
      return true;
    }
  }
  return false;
}

inline bool has_flag(const std::vector<std::string> &args,
                     const std::string &flag) {
  for (const auto &a : args)
    if (a == flag)
      return true;
  return false;
}

// Positional (non-option) arguments. Any dash-prefixed token is treated as
// a flag (unknown flags are ignored, like most simple tools). `flags_with_value`
// lists flags that consume the following token as their value (e.g. "-n" for
// "-n 5"); attached forms ("-n5", "-d' '") carry their value inline.
inline std::vector<std::string> positionals(const std::vector<std::string> &args,
                                            const std::vector<std::string> &flags_with_value) {
  std::vector<std::string> out;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &a = args[i];
    if (a.size() > 1 && a[0] == '-' && !std::isdigit((unsigned char)a[1])) {
      for (const auto &f : flags_with_value)
        if (a == f) ++i; // separate value token: skip it
      continue;
    }
    out.push_back(a);
  }
  return out;
}

// ── commands ───────────────────────────────────────────────────────────────
// Signature: (args, stdin) -> output. Return value is stdout; errors are
// written to the returned string with an "Error:" prefix (kept simple for
// the LLM to read).

inline std::string cmd_ls(const std::vector<std::string> &args,
                          const std::string &) {
  bool long_fmt = has_flag(args, "-l");
  bool all = has_flag(args, "-a");
  auto paths = positionals(args, {});
  if (paths.empty())
    paths.push_back(".");
  std::string out;
  for (const auto &p : paths) {
    fs::path dir;
    std::string err = resolve(p, dir);
    if (!err.empty())
      return err;
    if (!fs::exists(dir))
      return "ls: cannot access '" + p + "': No such file or directory";
    if (!fs::is_directory(dir)) {
      out += dir.filename().string() + "\n";
      continue;
    }
    if (paths.size() > 1)
      out += p + ":\n";
    std::vector<fs::directory_entry> entries;
    for (auto it = fs::directory_iterator(dir);
         it != fs::directory_iterator(); ++it) {
      const auto &e = *it;
      if (!all && e.path().filename().string().starts_with("."))
        continue;
      std::string name = e.path().filename().string();
      std::error_code ec;
      if (long_fmt) {
        auto sz = fs::file_size(e.path(), ec);
        char perm[11] = "----------";
        perm[0] = fs::is_directory(e.path(), ec) ? 'd' : '-';
        std::string line(10, ' ');
        line += perm;
        line += " 1 1 ";
        line += std::to_string(ec ? 0 : sz);
        line += " " + name;
        out += line + "\n";
      } else {
        if (fs::is_directory(e.path(), ec))
          name += "/";
        out += name + "\n";
      }
    }
  }
  return out.empty() ? "(empty)" : out;
}

// When no file operands are given, commands read from stdin. `content`
// receives the input and `from_stdin` is set.
inline bool use_stdin(const std::vector<std::string> &args,
                      const std::vector<std::string> &flags_with_value,
                      const std::string &stdin_data, std::string &content) {
  auto paths = positionals(args, flags_with_value);
  if (!paths.empty())
    return false;
  content = stdin_data;
  return true;
}

inline std::string cmd_cat(const std::vector<std::string> &args,
                           const std::string &stdin_data) {
  std::string content;
  if (use_stdin(args, {}, stdin_data, content))
    return content;
  auto paths = positionals(args, {});
  std::string out;
  for (const auto &p : paths) {
    fs::path f;
    std::string err = resolve(p, f);
    if (!err.empty())
      return err;
    std::string content;
    if ((err = read_all(f, content)) != "")
      return err + " (cat " + p + ")";
    out += content;
    if (!out.empty() && out.back() != '\n')
      out += "\n";
  }
  return out;
}

inline std::string head_lines(std::string content, int n) {
  auto lines = split_lines(content);
  std::string out;
  for (size_t i = 0; i < lines.size() && i < (size_t)n; ++i)
    out += lines[i] + "\n";
  return out.empty() ? "(empty)" : out;
}

inline std::string cmd_head(const std::vector<std::string> &args,
                            const std::string &stdin_data) {
  int n = 10;
  opt_int(args, "-n", n);
  std::string content;
  if (use_stdin(args, {"-n"}, stdin_data, content))
    return head_lines(content, n);
  auto paths = positionals(args, {"-n"});
  std::string out;
  for (const auto &p : paths) {
    fs::path f;
    std::string err = resolve(p, f);
    if (!err.empty())
      return err;
    if ((err = read_all(f, content)) != "")
      return err + " (head " + p + ")";
    out += head_lines(content, n);
  }
  return out.empty() ? "(empty)" : out;
}

inline std::string tail_lines(std::string content, int n) {
  auto lines = split_lines(content);
  size_t start = lines.size() > (size_t)n ? lines.size() - (size_t)n : 0;
  std::string out;
  for (size_t i = start; i < lines.size(); ++i)
    out += lines[i] + "\n";
  return out.empty() ? "(empty)" : out;
}

inline std::string cmd_tail(const std::vector<std::string> &args,
                            const std::string &stdin_data) {
  int n = 10;
  opt_int(args, "-n", n);
  std::string content;
  if (use_stdin(args, {"-n"}, stdin_data, content))
    return tail_lines(content, n);
  auto paths = positionals(args, {"-n"});
  std::string out;
  for (const auto &p : paths) {
    fs::path f;
    std::string err = resolve(p, f);
    if (!err.empty())
      return err;
    if ((err = read_all(f, content)) != "")
      return err + " (tail " + p + ")";
    out += tail_lines(content, n);
  }
  return out.empty() ? "(empty)" : out;
}

inline void wc_count(const std::string &content, long &l, long &w, long &c) {
  l = 0;
  w = 0;
  c = (long)content.size();
  {
    std::istringstream ls(content);
    std::string line;
    while (std::getline(ls, line))
      ++l;
    std::istringstream ws(content);
    std::string word;
    while (ws >> word)
      ++w;
  }
}

inline std::string wc_format(long l, long w, long c, bool only_l, bool only_w,
                             bool only_c, const std::string &name) {
  // No flag: print all three. With flags: print only the requested fields
  // (in line, word, byte order), like POSIX wc.
  bool any = only_l || only_w || only_c;
  std::vector<std::string> fields;
  if (!any || only_l) fields.push_back(std::to_string(l));
  if (!any || only_w) fields.push_back(std::to_string(w));
  if (!any || only_c) fields.push_back(std::to_string(c));
  std::string out;
  for (size_t i = 0; i < fields.size(); ++i)
    out += (i ? " " : "") + fields[i];
  if (!name.empty())
    out += "  " + name;
  out += "\n";
  return out;
}

inline std::string cmd_wc(const std::vector<std::string> &args,
                          const std::string &stdin_data) {
  bool only_l = has_flag(args, "-l"), only_w = has_flag(args, "-w"),
       only_c = has_flag(args, "-c");
  std::string content;
  if (use_stdin(args, {}, stdin_data, content)) {
    long l, w, c;
    wc_count(content, l, w, c);
    return wc_format(l, w, c, only_l, only_w, only_c, "");
  }
  auto paths = positionals(args, {});
  std::string out;
  long total_l = 0, total_w = 0, total_c = 0;
  for (const auto &p : paths) {
    fs::path f;
    std::string err = resolve(p, f);
    if (!err.empty())
      return err;
    if ((err = read_all(f, content)) != "")
      return err + " (wc " + p + ")";
    long l = 0, w = 0, c = 0;
    wc_count(content, l, w, c);
    total_l += l;
    total_w += w;
    total_c += c;
    if (paths.size() > 1)
      out += wc_format(l, w, c, only_l, only_w, only_c, p);
  }
  if (paths.size() == 1)
    out += wc_format(total_l, total_w, total_c, only_l, only_w, only_c, "");
  else
    out += wc_format(total_l, total_w, total_c, only_l, only_w, only_c,
                     "total");
  return out;
}

inline std::string cmd_grep(const std::vector<std::string> &args,
                            const std::string &stdin_data) {
  bool ignore_case = has_flag(args, "-i");
  bool invert = has_flag(args, "-v");
  bool show_line = has_flag(args, "-n");
  bool recursive = has_flag(args, "-r") || has_flag(args, "-R");
  std::string include_glob;
  std::vector<std::string> rest;
  for (const auto &a : args) {
    if (a.rfind("--include=", 0) == 0) {
      include_glob = a.substr(10);
    } else if (a.size() == 2 && a[0] == '-' &&
               (a[1] == 'i' || a[1] == 'v' || a[1] == 'n' || a[1] == 'r' ||
                a[1] == 'R')) {
      // boolean flag, already handled above
    } else {
      rest.push_back(a);
    }
  }
  if (rest.empty())
    return "grep: usage: grep [-i] [-v] [-n] [-r] [--include=GLOB] PATTERN [PATH...]\n"
           "(with no PATH, the pattern is searched against piped input)";
  std::string pattern = rest[0];
  std::regex re;
  try {
    std::regex::flag_type flags = std::regex::extended;
    if (ignore_case)
      flags |= std::regex::icase;
    re = std::regex(pattern, flags);
  } catch (const std::exception &e) {
    return std::string("grep: invalid pattern: ") + e.what();
  }

  // rest[0] is the pattern; paths are rest[1..]. If no path was given,
  // search piped stdin instead of a file.
  bool from_stdin = (rest.size() == 1);

  // Collect files to search.
  std::vector<fs::path> files;
  std::string stdin_content;
  for (size_t i = 1; i < rest.size(); ++i) {
    fs::path p;
    std::string err = resolve(rest[i], p);
    if (!err.empty())
      return err;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      if (!recursive)
        return "grep: " + rest[i] + ": is a directory (use -r)";
      for (auto it = fs::recursive_directory_iterator(
               p, fs::directory_options::skip_permission_denied);
           it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file())
          continue;
        if (!include_glob.empty() &&
            !glob_match(include_glob, it->path().filename().string()))
          continue;
        files.push_back(it->path());
      }
    } else {
      files.push_back(p);
    }
  }

  bool multi = files.size() > 1;
  fs::path root;
  try {
    root = fs::canonical(workspace());
  } catch (...) {
  }
  std::string out;
  if (from_stdin) {
    stdin_content = stdin_data;
  }
  auto search_content = [&](const std::string &content, const std::string &label) {
    auto lines = split_lines(content);
    for (size_t li = 0; li < lines.size(); ++li) {
      bool match = std::regex_search(lines[li], re);
      if (match == invert)
        continue;
      std::string prefix;
      if (multi && !label.empty())
        prefix = label + ":";
      if (show_line)
        prefix += std::to_string(li + 1) + ":";
      out += prefix + lines[li] + "\n";
    }
  };
  if (from_stdin) {
    search_content(stdin_content, "");
  }
  for (const auto &f : files) {
    std::string content;
    if (read_all(f, content) != "")
      continue; // skip unreadable
    std::string label;
    if (multi) {
      std::string rel = f.string();
      if (!root.empty() && rel.starts_with(root.string()))
        rel = rel.substr(root.string().size() + 1);
      label = rel;
    }
    search_content(content, label);
  }
  return out.empty() ? "(no matches)" : out;
}

inline std::string cmd_find(const std::vector<std::string> &args,
                            const std::string &) {
  std::string name_glob = "*";
  std::vector<std::string> rest;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "-name" && i + 1 < args.size())
      name_glob = args[++i];
    else
      rest.push_back(args[i]);
  }
  fs::path start;
  std::string err = resolve(rest.empty() ? "." : rest[0], start);
  if (!err.empty())
    return err;
  if (!fs::exists(start))
    return "find: '" + (rest.empty() ? std::string(".") : rest[0]) +
           "': No such file or directory";
  fs::path root = fs::canonical(workspace());
  std::string out;
  auto visit = [&](const fs::path &p) {
    if (glob_match(name_glob, p.filename().string())) {
      std::string rel = p.string();
      if (rel.starts_with(root.string()))
        rel = rel.substr(root.string().size() + 1);
      out += rel + "\n";
    }
  };
  std::error_code ec;
  if (fs::is_directory(start, ec)) {
    for (auto it = fs::recursive_directory_iterator(
             start, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it)
      visit(it->path());
  } else {
    visit(start);
  }
  return out.empty() ? "(no matches)" : out;
}

inline std::string cmd_mkdir(const std::vector<std::string> &args,
                             const std::string &) {
  auto paths = positionals(args, {});
  if (paths.empty())
    return "mkdir: missing operand";
  for (const auto &p : paths) {
    fs::path d;
    std::string err = resolve(p, d);
    if (!err.empty())
      return err;
    std::error_code ec;
    if (has_flag(args, "-p"))
      fs::create_directories(d, ec);
    else
      fs::create_directory(d, ec);
    if (ec && !fs::exists(d))
      return "mkdir: cannot create directory '" + p + "': " + ec.message();
  }
  return "";
}

inline std::string cmd_rm(const std::vector<std::string> &args,
                          const std::string &) {
  bool recursive = has_flag(args, "-r") || has_flag(args, "-R");
  auto paths = positionals(args, {});
  if (paths.empty())
    return "rm: missing operand";
  for (const auto &p : paths) {
    fs::path f;
    std::string err = resolve(p, f);
    if (!err.empty())
      return err;
    std::error_code ec;
    if (fs::is_directory(f, ec)) {
      if (!recursive)
        return "rm: cannot remove '" + p + "': Is a directory (use -r)";
      fs::remove_all(f, ec);
    } else {
      fs::remove(f, ec);
    }
    if (ec && fs::exists(f, ec))
      return "rm: cannot remove '" + p + "': " + ec.message();
  }
  return "";
}

inline std::string cmd_cp(const std::vector<std::string> &args,
                          const std::string &) {
  auto paths = positionals(args, {});
  if (paths.size() < 2)
    return "cp: usage: cp SRC DST";
  const std::string &src_s = paths[0];
  const std::string &dst_s = paths[paths.size() - 1];
  fs::path src, dst;
  std::string err = resolve(src_s, src);
  if (!err.empty())
    return err;
  if ((err = resolve(dst_s, dst)) != "")
    return err;
  if (!fs::exists(src))
    return "cp: cannot copy '" + src_s + "': No such file";
  std::error_code ec;
  if (fs::is_directory(dst, ec))
    dst = dst / src.filename();
  if (fs::is_directory(src, ec)) {
    fs::copy(src, dst, fs::copy_options::recursive |
                           fs::copy_options::update_existing,
             ec);
  } else {
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  }
  if (ec)
    return "cp: " + ec.message();
  return "";
}

inline std::string cmd_mv(const std::vector<std::string> &args,
                          const std::string &) {
  auto paths = positionals(args, {});
  if (paths.size() < 2)
    return "mv: usage: mv SRC DST";
  fs::path src, dst;
  std::string err = resolve(paths[0], src);
  if (!err.empty())
    return err;
  if ((err = resolve(paths[paths.size() - 1], dst)) != "")
    return err;
  if (!fs::exists(src))
    return "mv: cannot move '" + paths[0] + "': No such file";
  std::error_code ec;
  if (fs::is_directory(dst, ec) && !fs::is_directory(src, ec))
    dst = dst / src.filename();
  fs::rename(src, dst, ec);
  if (ec)
    return "mv: " + ec.message();
  return "";
}

inline std::string cmd_touch(const std::vector<std::string> &args,
                             const std::string &) {
  auto paths = positionals(args, {});
  if (paths.empty())
    return "touch: missing operand";
  for (const auto &p : paths) {
    fs::path f;
    std::string err = resolve(p, f);
    if (!err.empty())
      return err;
    std::error_code ec;
    if (!f.parent_path().empty())
      fs::create_directories(f.parent_path(), ec);
    std::ofstream f_out(f, std::ios::app);
    if (!f_out)
      return "touch: cannot touch '" + p + "'";
    f_out.close();
  }
  return "";
}

inline std::string cmd_pwd(const std::vector<std::string> &,
                           const std::string &) {
  try {
    return fs::canonical(workspace()).string() + "\n";
  } catch (...) {
    return workspace() + "\n";
  }
}

inline std::string cmd_date(const std::vector<std::string> &args,
                            const std::string &) {
  auto paths = positionals(args, {});
  std::string fmt = paths.empty() ? "%a %b %e %H:%M:%S %Y" : paths[0];
  time_t t = time(nullptr);
  struct tm tmv;
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[128];
  strftime(buf, sizeof(buf), fmt.c_str(), &tmv);
  return std::string(buf) + "\n";
}

inline std::string cmd_echo(const std::vector<std::string> &args,
                            const std::string &) {
  bool no_nl = has_flag(args, "-n");
  auto rest = positionals(args, {});
  std::string out;
  for (size_t i = 0; i < rest.size(); ++i) {
    if (i)
      out += " ";
    out += rest[i];
  }
  if (!no_nl)
    out += "\n";
  return out;
}

inline std::string cmd_sort(const std::vector<std::string> &args,
                            const std::string &stdin_data) {
  bool reverse = has_flag(args, "-r");
  bool numeric = has_flag(args, "-n");
  auto paths = positionals(args, {});
  std::string content;
  if (paths.empty()) {
    content = stdin_data;
  } else {
    fs::path f;
    std::string err = resolve(paths[0], f);
    if (!err.empty())
      return err;
    if ((err = read_all(f, content)) != "")
      return err;
  }
  auto lines = split_lines(content);
  std::sort(lines.begin(), lines.end(),
            [&](const std::string &a, const std::string &b) {
              bool less = numeric ? (std::atof(a.c_str()) < std::atof(b.c_str()))
                                  : (a < b);
              return reverse ? !less : less;
            });
  std::string out;
  for (const auto &l : lines)
    out += l + "\n";
  return out.empty() ? "(empty)" : out;
}

inline std::string cmd_uniq(const std::vector<std::string> &args,
                            const std::string &stdin_data) {
  bool count = has_flag(args, "-c");
  auto paths = positionals(args, {});
  std::string content;
  if (paths.empty()) {
    content = stdin_data;
  } else {
    fs::path f;
    std::string err = resolve(paths[0], f);
    if (!err.empty())
      return err;
    if ((err = read_all(f, content)) != "")
      return err;
  }
  auto lines = split_lines(content);
  std::string out;
  const std::string *prev = nullptr;
  long run = 0;
  auto flush = [&]() {
    if (prev) {
      if (count)
        out += "     " + std::to_string(run) + " " + *prev + "\n";
      else
        out += *prev + "\n";
    }
  };
  for (const auto &l : lines) { // `prev` points into `lines`, which outlives this loop
    if (prev && l == *prev) {
      ++run;
    } else {
      flush();
      prev = &l;
      run = 1;
    }
  }
  flush();
  return out.empty() ? "(empty)" : out;
}

inline std::string cmd_cut(const std::vector<std::string> &args,
                           const std::string &stdin_data) {
  char delim = '\t';
  int field = -1;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &a = args[i];
    if (a == "-d" && i + 1 < args.size() && args[i + 1].size() == 1)
      delim = args[i + 1][0];
    else if (a.size() == 3 && a[0] == '-' && a[1] == 'd')
      delim = a[2];
    if (a == "-f" && i + 1 < args.size())
      field = std::atoi(args[i + 1].c_str());
    else if (a.size() == 3 && a[0] == '-' && a[1] == 'f')
      field = std::atoi(a.c_str() + 2);
  }
  if (field < 1)
    return "cut: you must specify a field (-f N)";
  auto paths = positionals(args, {"-d", "-f"});
  std::string content;
  if (paths.empty()) {
    content = stdin_data;
  } else {
    fs::path f;
    std::string err = resolve(paths[0], f);
    if (!err.empty())
      return err;
    if ((err = read_all(f, content)) != "")
      return err;
  }
  std::string out;
  for (const auto &line : split_lines(content)) {
    int cur = 0;
    size_t start = 0;
    for (size_t i = 0;; ++i) {
      size_t pos = line.find(delim, start);
      if (++cur == field) {
        out += (pos == std::string::npos ? line.substr(start)
                                         : line.substr(start, pos - start));
        break;
      }
      if (pos == std::string::npos)
        break;
      start = pos + 1;
    }
    out += "\n";
  }
  return out.empty() ? "(empty)" : out;
}

inline std::string cmd_basename(const std::vector<std::string> &args,
                                const std::string &) {
  auto paths = positionals(args, {});
  if (paths.empty())
    return "basename: missing operand";
  std::string name = fs::path(paths[0]).filename().string();
  if (paths.size() > 1 && name.size() >= paths[1].size() &&
      name.compare(name.size() - paths[1].size(), paths[1].size(),
                   paths[1]) == 0)
    name.erase(name.size() - paths[1].size());
  return name + "\n";
}

inline std::string cmd_dirname(const std::vector<std::string> &args,
                               const std::string &) {
  auto paths = positionals(args, {});
  if (paths.empty())
    return "dirname: missing operand";
  fs::path p(paths[0]);
  std::string parent = p.parent_path().string();
  return (parent.empty() ? "." : parent) + "\n";
}

struct Cmd {
  std::string (*fn)(const std::vector<std::string> &, const std::string &);
  const char *summary; // map key carries the command name
};

inline const std::map<std::string, Cmd> &commands() {
  static const std::map<std::string, Cmd> cmds = {
      {"ls", {cmd_ls, "list directory contents"}},
      {"cat", {cmd_cat, "print file contents"}},
      {"head", {cmd_head, "first N lines of a file"}},
      {"tail", {cmd_tail, "last N lines of a file"}},
      {"wc", {cmd_wc, "count lines/words/bytes"}},
      {"grep", {cmd_grep, "search lines matching a pattern (-r for recursive)"}},
      {"find", {cmd_find, "find files by name glob (-name GLOB)"}},
      {"mkdir", {cmd_mkdir, "create directories (-p)"}},
      {"rm", {cmd_rm, "remove files (-r for directories)"}},
      {"cp", {cmd_cp, "copy files"}},
      {"mv", {cmd_mv, "move/rename files"}},
      {"touch", {cmd_touch, "create empty file / update timestamp"}},
      {"pwd", {cmd_pwd, "print workspace directory"}},
      {"date", {cmd_date, "current date/time (strftime format)"}},
      {"echo", {cmd_echo, "print text"}},
      {"sort", {cmd_sort, "sort lines (-r reverse, -n numeric)"}},
      {"uniq", {cmd_uniq, "deduplicate adjacent lines (-c counts)"}},
      {"cut", {cmd_cut, "extract fields (-d DELIM -f N)"}},
      {"basename", {cmd_basename, "last path component"}},
      {"dirname", {cmd_dirname, "parent directory"}},
  };
  return cmds;
}

inline std::string cmd_which(const std::vector<std::string> &args,
                             const std::string &) {
  auto rest = positionals(args, {});
  if (rest.empty())
    return "usage: which COMMAND";
  for (const auto &c : rest) {
    auto it = commands().find(c);
    if (it != commands().end())
      // Report as a built-in path so the model sees a normal-looking result.
      return std::string("/usr/bin/") + c + "\n";
  }
  return "which: " + rest[0] + ": not found (mobile shell has no such command)";
}

// ── tokenizer & pipeline ───────────────────────────────────────────────────

inline std::vector<std::string> tokenize(const std::string &s) {
  std::vector<std::string> tokens;
  std::string cur;
  bool in_s = false, in_d = false, has = false;
  for (char c : s) {
    if (c == '\'' && !in_d) {
      in_s = !in_s;
      has = true;
    } else if (c == '"' && !in_s) {
      in_d = !in_d;
      has = true;
    } else if (std::isspace((unsigned char)c) && !in_s && !in_d) {
      if (has || !cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
        has = false;
      }
    } else {
      cur += c;
      has = true;
    }
  }
  if (has || !cur.empty())
    tokens.push_back(cur);
  return tokens;
}

// Split a token stream on top-level pipes into stages.
inline std::vector<std::vector<std::string>> split_pipeline(
    const std::vector<std::string> &tokens) {
  std::vector<std::vector<std::string>> stages;
  std::vector<std::string> cur;
  for (const auto &t : tokens) {
    if (t == "|") {
      if (!cur.empty())
        stages.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(t);
    }
  }
  if (!cur.empty())
    stages.push_back(cur);
  return stages;
}

inline std::string run_stage(const std::vector<std::string> &tokens,
                             const std::string &stdin_data) {
  if (tokens.empty())
    return "";
  std::string cmd = tokens[0];
  std::vector<std::string> args(tokens.begin() + 1, tokens.end());

  if (cmd == "which")
    return cmd_which(args, stdin_data);
  if (cmd == "true")
    return "";
  if (cmd == "false")
    return "Error: exit status 1";

  auto it = commands().find(cmd);
  if (it == commands().end()) {
    std::string avail;
    for (const auto &[n, c] : commands())
      avail += (avail.empty() ? "" : ", ") + n;
    return "sh: " + cmd + ": not found. Available commands: " + avail +
           ". (No real shell on mobile — no redirection, &&, or variables. "
           "Use the read_file/write_file/edit_file/list_dir tools for file "
           "work.)";
  }
  return truncate(it->second.fn(args, stdin_data));
}

inline std::string run(const std::string &command_line) {
  std::string trimmed = command_line;
  while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front()))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))
    trimmed.pop_back();
  if (trimmed.empty())
    return "";

  auto tokens = tokenize(trimmed);
  auto stages = split_pipeline(tokens);
  std::string input;
  std::string output;
  for (size_t i = 0; i < stages.size(); ++i) {
    output = run_stage(stages[i], input);
    input = output;
    if (i + 1 < stages.size() &&
        (output.starts_with("Error:") ||
         output.starts_with("sh: ")))
      break; // don't pipe errors into the next stage
  }
  return output.empty() ? "" : output;
}

} // namespace mshell

// ── Tool wrapper ───────────────────────────────────────────────────────────

class MobileShellTool : public Tool {
public:
  std::string name() const override { return "exec"; }

  std::string description() const override {
    return "Execute a shell-like command using built-in C++ implementations "
           "(no real shell on this device). Supports: ls, cat, head, tail, wc, "
           "grep (-i -v -n -r --include), find (-name), mkdir (-p), rm (-r), "
           "cp, mv, touch, pwd, date, echo, sort, uniq, cut, basename, dirname, "
           "which — and pipelines with '|'. Paths are relative to the workspace "
           "and confined to it. NOT supported: redirection, &&, ||, variables, "
           "subshells, or arbitrary binaries.";
  }

  std::string schema() const override {
    return R"===({"type":"function","function":{"name":"exec","description":"Execute a shell-like command with built-in implementations (ls, cat, grep, find, wc, head, tail, sort, uniq, cut, mkdir, rm, cp, mv, touch, pwd, date, echo, ...). Pipelines with | work. Paths are workspace-relative and sandboxed. No redirection, &&, variables, or external binaries.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"The command to run, e.g. 'grep -n keyword memory/HISTORY.md' or 'ls memory | sort'"}},"required":["command"]}}})===";
  }

  std::string execute(const std::map<std::string, std::string> &args) override {
    auto it = args.find("command");
    if (it == args.end())
      return "Error: missing 'command' argument";
    return execute(it->second);
  }

  std::string execute(const std::string &input) override {
    spdlog::info("[mobile-shell] exec: {}", input);
    try {
      std::string result = mshell::run(input);
      spdlog::debug("[mobile-shell] output (first 200 chars): {}",
                    result.substr(0, 200));
      return result.empty() ? "(no output)" : result;
    } catch (const std::exception &e) {
      return std::string("Error: ") + e.what();
    }
  }
};

#endif // MC_MOBILE
