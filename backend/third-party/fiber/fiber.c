#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__APPLE__)
#include <malloc.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "include/fiber.h"
#include "include/context.h"
#include "include/chain.h"

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#ifdef _MSC_VER
#define __thread_local   __declspec(thread)
#else
#define __thread_local   __thread
#ifndef __forceinline
#define __forceinline    inline __attribute__((always_inline))
#endif
#endif

#ifndef _WIN32
#ifdef __APPLE__
static inline void* _apple_aligned_alloc(size_t alignment, size_t size) {
    void *p;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}
#define _aligned_malloc(n, a) _apple_aligned_alloc(a, (n + a - 1) & ~(a - 1))
#elif defined(__ANDROID__)
#define _aligned_malloc(n, a) memalign(a, (n + a - 1) & ~(a - 1))
#else
#define _aligned_malloc(n, a) aligned_alloc(a, (n + a - 1) & ~(a - 1))
#endif
#define _aligned_free(p)      free(p)
#endif

/*
 * MASK_SYSTEM_STACK: high bit of stacksize used to tag system-allocated stacks.
 *   - If set: system owns the stack → GC/destroy will free it
 *   - If clear: user provided the stack → GC/destroy will NOT free it
 *
 * To recover the real stacksize: stacksize & ~MASK_SYSTEM_STACK
 */
#define MASK_SYSTEM_STACK ((size_t)1 << (sizeof(size_t) * 8 - 1))

// Thread-local context structure
typedef struct {
    _CHAIN_HEAD(FibReadyList,    FibTCB)  readylist;
    _CHAIN_HEAD(FibDeadList,     FibTCB)  dead_list;
    FCHAIN_HEAD(FibWatchdogList, FibTCB)  watchdoglist;
    FibTCB *                            current_fiber;
    FibTCB *                            main_fiber;
    volatile int64_t                    nLocalFibTasks;
    bool                                (*fiber_scheduler_callback)(void *);
    void *                              fiber_scheduler_arg;
} FibThreadCtx;

static __thread_local FibThreadCtx g_ctx;

static inline uint64_t fiber_time_us() {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int has_freq = 0;
    if (!has_freq) {
        QueryPerformanceFrequency(&freq);
        has_freq = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000ULL / freq.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
}

static void fiber_watchdog_insert(FibTCB * tcb, int64_t usec) {
    int64_t  delta = usec;
    FibTCB * after = FCHAIN_FIRST(&g_ctx.watchdoglist);
    while (FCHAIN_NEXT(after, timeout_node)) {
        if (delta < after->delta_interval) {
            after->delta_interval -= delta;
            break;
        }
        delta -= after->delta_interval;
        after  = FCHAIN_NEXT(after, timeout_node);
    }
    tcb->delta_interval = delta;
    tcb->state          = FIB_STATE_SLEEP;
    FCHAIN_INSERT_BEFORE(after, tcb, FibTCB, timeout_node);
}

static void fiber_watchdog_remove(FibTCB * tcb) {
    FibTCB * nxt = FCHAIN_NEXT(tcb, timeout_node);
    if (FCHAIN_NEXT(nxt, timeout_node)) {
        nxt->delta_interval += tcb->delta_interval;
    }
    FCHAIN_REMOVE(tcb, FibTCB, timeout_node);
}

static void fiber_watchdog_tickle(int64_t gap) {
    FibTCB * tcb, * nxt;
    FCHAIN_FOREACH_SAFE(tcb, &g_ctx.watchdoglist, timeout_node, nxt) {
        if (tcb->delta_interval > gap) {
            tcb->delta_interval -= gap;
            break;
        }
        gap -= tcb->delta_interval;
        tcb->delta_interval  = 0;
        FCHAIN_REMOVE(tcb, FibTCB, timeout_node);

        tcb->state = FIB_STATE_READY;
        _CHAIN_INSERT_BEFORE(g_ctx.main_fiber, tcb);
    }
}

/**
 * @brief Internal: clean up finished fibers from the dead list.
 *
 * Only frees the stack if MASK_SYSTEM_STACK is set in stacksize,
 * meaning the system allocated it. User-provided stacks are left alone.
 */
static void fiber_cleanup_dead() {
    if (_CHAIN_IS_EMPTY(&g_ctx.dead_list)) return;
    FibTCB * tcb, * nxt;
    _CHAIN_FOREACH_SAFE(tcb, &g_ctx.dead_list, FibTCB, nxt) {
        _CHAIN_REMOVE(tcb);

        /* Only free the stack if it was system-allocated */
        if (tcb->stacksize & MASK_SYSTEM_STACK) {
            _aligned_free(tcb->stackaddr);
        }
        tcb->stackaddr = NULL;

        /* Always free the TCB (it's always system-allocated) */
        _aligned_free(tcb);
    }
}

static void fiber_sched() {
    FibTCB * prev      = g_ctx.current_fiber;
    FibTCB * the_next  = NULL;

    if (_CHAIN_FIRST(&g_ctx.readylist) == _CHAIN_LAST(&g_ctx.readylist) &&
        _CHAIN_FIRST(&g_ctx.readylist) == g_ctx.main_fiber) {
        fiber_cleanup_dead();
        if (g_ctx.fiber_scheduler_callback) {
            g_ctx.fiber_scheduler_callback(g_ctx.fiber_scheduler_arg);
        }
    }

    FibTCB * first     = _CHAIN_FIRST(&g_ctx.readylist);
    the_next           = (FibTCB *)first;
    _CHAIN_REMOVE(the_next);

    if (unlikely(the_next == g_ctx.main_fiber)) {
        _CHAIN_INSERT_TAIL(&g_ctx.readylist, the_next);
    } else {
        _CHAIN_INSERT_BEFORE(g_ctx.main_fiber, the_next);
        fiber_cleanup_dead();
    }

    g_ctx.current_fiber = the_next;
    the_next->state     = FIB_STATE_RUNNING;

    if (prev != the_next) {
        swap_context(&prev->regs, &the_next->regs);
    }
}

static void FibMainBridge(FibTCB * tcb) {
    /* callback: onTaskStartup */
    if (tcb->onTaskStartup) {
        tcb->onTaskStartup(tcb);
    }

    /* call user entry */
    if (tcb->entry) {
        tcb->entry(tcb->arg);
    }

    /* callback: onTaskCleanup */
    if (tcb->onTaskCleanup) {
        tcb->onTaskCleanup(tcb);
    }

    tcb->state           = FIB_STATE_DEAD;
    g_ctx.nLocalFibTasks--;
    _CHAIN_REMOVE(tcb);
    _CHAIN_APPEND(&g_ctx.dead_list, tcb);
    g_ctx.main_fiber->state = FIB_STATE_RUNNING;
    g_ctx.current_fiber     = g_ctx.main_fiber;
    FibCTX dummy         = {0};
    swap_context(&dummy, &g_ctx.main_fiber->regs);
}

bool FiberGlobalStartup() {
    /* Placeholder for future M:N model global initialization */
    return true;
}

bool FiberThreadStartup() {
    if (g_ctx.main_fiber != NULL) return true;
    _CHAIN_INIT_EMPTY(&g_ctx.readylist);
    _CHAIN_INIT_EMPTY(&g_ctx.dead_list);
    FCHAIN_INIT_EMPTY(&g_ctx.watchdoglist, FibTCB, timeout_node);
    size_t   size        = (sizeof(FibTCB) + 15) & ~15ULL;
    g_ctx.main_fiber     = (FibTCB*)_aligned_malloc(size, 16);
    if (!g_ctx.main_fiber) return false;
    memset(g_ctx.main_fiber, 0, sizeof(FibTCB));
    g_ctx.main_fiber->state     = FIB_STATE_RUNNING;
    g_ctx.main_fiber->scheduler = g_ctx.main_fiber;
    /* main_fiber: stackaddr=NULL, stacksize=0 (no MASK_SYSTEM_STACK) → never freed */
    g_ctx.current_fiber  = g_ctx.main_fiber;
    g_ctx.nLocalFibTasks = 1;
    _CHAIN_INSERT_TAIL(&g_ctx.readylist, g_ctx.main_fiber);
    return true;
}

FibTCB * fiber_create(void * (*func)(void*), void* arg, void* stackaddr, size_t stack_size) {
    if (unlikely(g_ctx.main_fiber == NULL)) FiberThreadStartup();
    size_t   size      = (sizeof(FibTCB) + 15) & ~15ULL;
    FibTCB * tcb       = (FibTCB*)_aligned_malloc(size, 16);
    if (!tcb) return NULL;
    memset(tcb, 0, sizeof(FibTCB));
    tcb->state          = FIB_STATE_SUSPENDED; // Initialize with SUSPENDED state

    if (stackaddr == NULL) {
        /* System-allocated stack */
        if (stack_size == 0) stack_size = 64 * 1024;
        stack_size     = (stack_size + 15) & ~15ULL;
        stackaddr      = _aligned_malloc(stack_size, 16);
        if (!stackaddr) { _aligned_free(tcb); return NULL; }
        /* Tag as system-owned */
        stack_size    |= MASK_SYSTEM_STACK;
    } else {
        /* User-provided stack: no MASK_SYSTEM_STACK → GC won't free it */
        stack_size     = (stack_size + 15) & ~15ULL;
    }

    tcb->stackaddr     = stackaddr;
    tcb->stacksize     = stack_size;
    tcb->entry         = func;
    tcb->arg           = arg;
    tcb->state         = FIB_STATE_READY; // Set to READY after setup
    tcb->scheduler     = g_ctx.main_fiber;

    size_t    real_size = stack_size & ~MASK_SYSTEM_STACK;
    uintptr_t stack_top = (uintptr_t)stackaddr + real_size;
    uint64_t * sp       = (uint64_t*)(stack_top & ~15ULL);
#if defined(__aarch64__)
    tcb->regs.x19      = (uint64_t)FibMainBridge;
    tcb->regs.x20      = (uint64_t)tcb;
    tcb->regs.lr       = (uint64_t)asm_taskmain;
    tcb->regs.sp       = (uint64_t)sp;
#elif defined(_WIN32)
    tcb->regs.reg_r12  = (uint64_t)FibMainBridge;
    tcb->regs.reg_r13  = (uint64_t)tcb;
    tcb->regs.reg_rip  = (uint64_t)asm_taskmain;
    tcb->regs.stack_base  = (uint64_t)sp;
    tcb->regs.stack_limit = (uint64_t)stackaddr;
    tcb->regs.reg_seh     = (uint64_t)-1;

    sp                -= 0;
    *(--sp)            = (uint64_t)asm_taskmain;
    tcb->regs.reg_rsp  = (uint64_t)sp;
#else
    tcb->regs.reg_r12  = (uint64_t)FibMainBridge;
    tcb->regs.reg_r13  = (uint64_t)tcb;
    tcb->regs.reg_rip  = (uint64_t)asm_taskmain;
    *(--sp)            = tcb->regs.reg_rip;
    tcb->regs.reg_rsp  = (uintptr_t)sp;
#endif
    g_ctx.nLocalFibTasks++;
    _CHAIN_INSERT_BEFORE(g_ctx.main_fiber, tcb);
    return tcb;
}

uint64_t fiber_resume(FibTCB * tcb) {
    if (unlikely(tcb == NULL || tcb->state != FIB_STATE_SUSPENDED)) return 0;
    uint64_t code        = tcb->yieldCode;
    tcb->state           = FIB_STATE_READY;
    _CHAIN_INSERT_BEFORE(g_ctx.main_fiber, tcb);
    return code;
}

FibTCB* fiber_suspend(uint64_t code) {
    FibTCB * tcb = g_ctx.current_fiber;
    if (unlikely(tcb == g_ctx.main_fiber)) return tcb;
    tcb->yieldCode = code;
    tcb->state     = FIB_STATE_SUSPENDED;
    _CHAIN_REMOVE(tcb); // Remove from readylist before scheduling
    fiber_sched();
    return tcb;
}

FibTCB* fiber_yield(uint64_t code) {
    g_ctx.current_fiber->yieldCode = code;
    if (g_ctx.current_fiber != g_ctx.main_fiber) g_ctx.current_fiber->state = FIB_STATE_READY;
    fiber_sched();
    return g_ctx.current_fiber;
}

void fiber_usleep(int64_t usec) {
    if (unlikely(g_ctx.current_fiber == g_ctx.main_fiber)) return;
    fiber_watchdog_insert(g_ctx.current_fiber, usec);
    _CHAIN_REMOVE(g_ctx.current_fiber);
    fiber_sched();
}

void fiber_destroy(FibTCB * tcb) {
    if (!tcb || tcb == g_ctx.main_fiber || tcb == g_ctx.current_fiber) return;

    FibState bstate = tcb->state;
    /* for SLEEP, remove from watchdog if exists  */
    if (bstate == FIB_STATE_SLEEP) {
        fiber_watchdog_remove(tcb);
    }

    /* remove from readylist, or deadlist if in one of them */
    if (bstate == FIB_STATE_READY     || 
        bstate == FIB_STATE_SUSPENDED || 
        bstate == FIB_STATE_DEAD) _CHAIN_REMOVE(tcb);
    
    /* Only free system-allocated stacks */
    if (tcb->stacksize & MASK_SYSTEM_STACK) {
        _aligned_free(tcb->stackaddr);
    }
    tcb->stackaddr = NULL;
    _aligned_free(tcb);
}

FibState fiber_status(FibTCB* tcb) { return tcb ? tcb->state : FIB_STATE_INVALID; }
FibTCB* fiber_ident() { return g_ctx.current_fiber; }

void * fiber_thread_entry(void * args) {
    if (unlikely(g_ctx.main_fiber == NULL)) return NULL;

    /* Extract scheduler callback from args if provided */
    fiber_scheduler_args_t * sched_args = (fiber_scheduler_args_t*)args;
    if (sched_args) {
        g_ctx.fiber_scheduler_callback = sched_args->fiberSchedulerCallback;
        g_ctx.fiber_scheduler_arg      = sched_args->args;
    }

    g_ctx.main_fiber->arg  = args;
    uint64_t prev_stmp     = fiber_time_us();
    while (g_ctx.nLocalFibTasks > 1) {
        uint64_t curr_stmp = fiber_time_us();
        int64_t gap        = curr_stmp - prev_stmp;
        if (gap > 0) { fiber_watchdog_tickle(gap); prev_stmp = curr_stmp; }
        fiber_yield(0);
    }
    fiber_cleanup_dead();
    return NULL;
}


