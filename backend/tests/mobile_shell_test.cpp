// Functional test for the mobile shell. Compile with -DMC_MOBILE.
// Usage: build against the backend sources (see MOBILE.md) and run.
#define MC_MOBILE
#include "tools/mobile_shell.hpp"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef WIN32
#include <cstdlib>
static void set_ws_env(const char *v) { setenv("WORKSPACE_DIR", v, 1); }
#else
static void set_ws_env(const char *v) { _putenv_s("WORKSPACE_DIR", v); }
#endif

int main() {
  const std::string ws = "C:/tmp/mc_shell_test";
  std::filesystem::remove_all(ws);
  std::filesystem::create_directories(ws + "/memory");
  set_ws_env(ws.c_str());

  // Seed some files.
  {
    std::ofstream f(ws + "/memory/HISTORY.md");
    f << "# History\n";
    for (int i = 1; i <= 20; ++i) f << "entry " << i << " alpha\n";
    f << "secret line with keyword\n";
    std::ofstream g(ws + "/notes.txt");
    g << "hello world\nhello miniclaw\nfoo bar baz\n";
    std::ofstream h(ws + "/dup.txt");
    h << "b\na\na\nb\nb\n";
  }

  int failures = 0;
  auto test = [&](const std::string &cmd, const std::string &expect_substr) {
    std::string r = mshell::run(cmd);
    bool ok = r.find(expect_substr) != std::string::npos;
    if (!ok) ++failures;
    std::cout << (ok ? "PASS" : "FAIL") << " | " << cmd << "\n       -> "
              << (r.size() > 160 ? r.substr(0, 160) + "..." : r) << "\n";
  };

  test("pwd", "mc_shell_test");
  test("ls", "memory/");
  test("ls memory", "HISTORY.md");
  test("cat notes.txt", "hello miniclaw");
  test("head -n 3 memory/HISTORY.md", "entry 1");
  test("tail -n 2 memory/HISTORY.md", "secret line with keyword");
  test("wc -l notes.txt", "3");
  test("wc notes.txt", "3");   // lines
  test("wc notes.txt", "7");   // words: 2+2+3
  test("grep keyword memory/HISTORY.md", "secret line");
  test("grep -n keyword memory/HISTORY.md", "22:secret");
  test("grep -r alpha memory | wc -l", "20");
  test("find . -name '*.md'", "HISTORY.md");
  test("echo hello world", "hello world");
  test("sort notes.txt", "foo bar baz");
  test("sort -r notes.txt", "hello world");
  test("cut -d' ' -f2 notes.txt | head -n 1", "world");
  test("cut -d ' ' -f2 notes.txt", "miniclaw");  // space-separated flag value
  test("echo a b c | wc -w", "3");
  test("basename memory/HISTORY.md", "HISTORY.md");
  test("dirname memory/HISTORY.md", "memory");
  test("which ls", "/usr/bin/ls");
  test("mkdir -p deep/nested", "");      // success -> empty output
  test("touch deep/nested/t.txt", "");
  test("cp notes.txt deep/copy.txt", "");
  test("cat deep/copy.txt", "hello miniclaw");
  test("mv deep/copy.txt deep/moved.txt", "");
  test("cat deep/moved.txt", "hello miniclaw");
  test("rm deep/moved.txt", "");
  test("cat /etc/passwd", "outside the app sandbox");
  test("cat ../config.yaml", "outside the app sandbox");
  test("ls /", "outside the app sandbox");
  test("rm -r ../../..", "outside the app sandbox");  // ancestor escape must fail
  test("nonexistent-cmd", "not found");
  test("rm -r deep", "");

  // date: must produce a plausible non-empty line
  {
    std::string r = mshell::run("date");
    bool ok = !r.empty() && r.find("Error") == std::string::npos;
    if (!ok) ++failures;
    std::cout << (ok ? "PASS" : "FAIL") << " | date (non-empty)\n       -> " << r << "\n";
  }

  // uniq -c via pipe
  test("sort dup.txt | uniq -c", "2 a");
  test("sort dup.txt | uniq -c", "3 b");

  std::cout << (failures ? "\nSOME TESTS FAILED\n" : "\nALL TESTS PASSED\n");
  return failures ? 1 : 0;
}
