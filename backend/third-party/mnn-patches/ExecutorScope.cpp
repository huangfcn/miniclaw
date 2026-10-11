//
//  ExecutorScope.cpp
//  MNN (MiniClaw patch — see backend/CMakeLists.txt)
//
//  Upstream uses `thread_local static Scope<ExecutorRef>` on non-Apple
//  platforms. On MSYS2/MinGW that is a problem: function-local thread_local
//  statics in a DLL are *emulated* TLS (libgcc _emutls_get_address). The
//  Scope object lives in a heap block owned by the emutls machinery, and its
//  destructor is registered via __cxa_thread_atexit, which MinGW runs from a
//  native Windows TLS callback (DLL_THREAD_DETACH).
//
//  At thread exit, libgcc's pthread-key destructor frees all of the thread's
//  emutls blocks BEFORE the native TLS callback runs ~Scope(). The destructor
//  then reads freed memory (refilled with ntdll's 0xFEEEFEEE allocation-fill
//  pattern) and frees a garbage vector-buffer pointer → STATUS_HEAP_CORRUPTION
//  (0xC0000374), fatal on any non-main thread.
//
//  Fix: keep the per-thread scope, but store it in plain heap memory and look
//  it up through a pthread key. The key's destructor (registered here) deletes
//  the scope during libwinpthread's own thread-exit key loop — no emutls block
//  is involved, so nothing else can free the object first. Semantics are
//  unchanged: one Scope per thread, EnterScope/ExitScope balanced per thread,
//  scope destroyed when the thread exits.

#include <thread>
#include <mutex>
#include <pthread.h>
#include <MNN/expr/Executor.hpp>
#include <MNN/expr/Scope.hpp>
#include <MNN/expr/ExecutorScope.hpp>

namespace MNN {
namespace Express {

typedef std::shared_ptr<Express::Executor> ExecutorRef;

static pthread_key_t gScopeKey = static_cast<pthread_key_t>(-1);
static std::once_flag gInitFlag;
// Fallback used only if pthread_key_create fails (should not happen): a single
// shared scope. Cross-thread scope stacks would then interleave, but that is
// strictly better than undefined behavior on an invalid key.
static Scope<ExecutorRef> gFallbackScope;

static void _scopeKeyDestructor(void* value) {
    delete static_cast<Scope<ExecutorRef>*>(value);
}

static Scope<ExecutorRef>* _getGlobalScope() {
    std::call_once(gInitFlag, []() {
        if (pthread_key_create(&gScopeKey, _scopeKeyDestructor) != 0) {
            gScopeKey = static_cast<pthread_key_t>(-1);
        }
    });
    if (gScopeKey == static_cast<pthread_key_t>(-1)) {
        return &gFallbackScope;
    }
    Scope<ExecutorRef>* scope = static_cast<Scope<ExecutorRef>*>(pthread_getspecific(gScopeKey));
    if (!scope) {
        scope = new Scope<ExecutorRef>;
        pthread_setspecific(gScopeKey, static_cast<void*>(scope));
    }
    return scope;
}

ExecutorScope::ExecutorScope(const std::shared_ptr<Executor>& current) {
    _getGlobalScope()->EnterScope(current);
}

ExecutorScope::ExecutorScope(const std::string& scope_name,
                             const std::shared_ptr<Executor>& current) {
    _getGlobalScope()->EnterScope(scope_name, current);
}

ExecutorScope::~ExecutorScope() {
    _getGlobalScope()->ExitScope();
}

const std::shared_ptr<Executor> ExecutorScope::Current() {
    auto exe = _getGlobalScope()->Content();
    if (exe) {
        return exe;
    }
    return Executor::getGlobalExecutor();
}

}  // namespace Express
}  // namespace MNN
