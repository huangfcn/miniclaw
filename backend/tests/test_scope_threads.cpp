// Regression test for the MSYS2/MinGW emutls thread-exit ordering bug in
// MNN's ExecutorScope (see third-party/mnn-patches/ExecutorScope.cpp).
//
// Pre-fix: each short-lived thread that entered an ExecutorScope crashed at
// exit with STATUS_HEAP_CORRUPTION (0xC0000374) — ~Scope() ran from MinGW's
// native TLS callback on the emutls block that libgcc had already freed.
// Post-fix: scopes live in plain heap memory owned by a pthread key; all
// threads exit cleanly.

#include <MNN/expr/Executor.hpp>
#include <MNN/expr/ExecutorScope.hpp>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
  // Warm the global executor on the main thread (also exercises the
  // process-lifetime scope that is never destroyed by a key destructor).
  {
    auto exe = MNN::Express::Executor::getGlobalExecutor();
    MNN::Express::ExecutorScope s(exe);
    (void)MNN::Express::ExecutorScope::Current();
  }

  const int kThreads = 200;
  std::atomic<int> done{0};
  std::vector<std::thread> pool;
  for (int i = 0; i < kThreads; ++i) {
    pool.emplace_back([&done]() {
      auto exe = MNN::Express::Executor::getGlobalExecutor();
      {
        MNN::Express::ExecutorScope s(exe);
        auto cur = MNN::Express::ExecutorScope::Current();
        if (!cur) {
          printf("FAIL: Current() returned null\n");
          exit(1);
        }
      }
      done.fetch_add(1);
    });
  }
  for (auto& t : pool)
    t.join();

  printf("scope-thread test: %d/%d threads exited cleanly\n",
         done.load(), kThreads);
  return done.load() == kThreads ? 0 : 1;
}
