#ifndef __FIBER_H__
#define __FIBER_H__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "context.h"
#include "chain.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_FIBER_LOCALDATAS (4)

typedef enum {
    FIB_STATE_INVALID = 0,
    FIB_STATE_SUSPENDED,  // Alive, but NOT in the ready list
    FIB_STATE_READY,      // Alive, and waiting in the ready list
    FIB_STATE_RUNNING,    // Currently executing
    FIB_STATE_SLEEP,      // Sleeping (in watchdog list)
    FIB_STATE_DEAD        // Finished executing
} FibState;

typedef struct FibTCB {
    /* ready / suspend / free queue */
    _CHAIN_ENTRY(FibTCB) node;

    /* CPU switching context */
    FibCTX               regs;
    
    /* watchdog / timeout queue */
    FCHAIN_ENTRY(FibTCB) timeout_node;
    int64_t              delta_interval;

    /* start point, arguments */
    void *               (*entry)(void *);
    void *               arg;
    
    /* stack address & size */
    void *               stackaddr;
    size_t               stacksize;

    /* yieldCode / scheduler */
    uint64_t             yieldCode;        /* saved by suspend, returned by resume */
    struct FibTCB *      scheduler;        /* points to scheduler fiber */

    /* Per-fiber lifecycle callbacks */
    bool                 (*onTaskStartup)(struct FibTCB *);
    bool                 (*onTaskCleanup)(struct FibTCB *);

    /* state & options (aligned to 8 bytes together) */
    FibState             state;            /* 4 bytes */
    int32_t              padding;          /* Maintain alignment */

    /* Fiber Local Storage */
    uint64_t             taskLocalStorages[MAX_FIBER_LOCALDATAS];
} FibTCB;
typedef struct FibTCB * fiber_t;

/**
 * @brief Initialize global fiber state. Call once at process startup.
 */
bool     FiberGlobalStartup();

/**
 * @brief Initialize the fiber environment for the current thread.
 *        Must be called once per thread before using other fiber_* functions.
 */
bool     FiberThreadStartup();

/**
 * @brief Create a new fiber.
 * 
 * @param func The entry function for the fiber.
 * @param arg  User argument passed to the entry function.
 * @param stackaddr  User-provided stack buffer, or NULL to let the system allocate.
 *                   If non-NULL, the caller owns the memory and must free it after
 *                   the fiber is destroyed. The system will NOT free user-provided stacks.
 * @param stack_size Stack size in bytes. If stackaddr is NULL and stack_size is 0,
 *                   a default size (64KB) is used.
 * @return FibTCB* Pointer to the new fiber TCB, or NULL on failure.
 */
FibTCB * fiber_create(void* (*func)(void*), void* arg, void* stackaddr, size_t stack_size);

/**
 * @brief Resume execution of a suspended fiber.
 *        If the fiber is newly created, it starts its entry function.
 *        If it was suspended via fiber_suspend, it resumes from the suspend point.
 * 
 * @param co The fiber to resume.
 * @return uint64_t The code passed to fiber_suspend/yield.
 */
uint64_t fiber_resume(FibTCB* co);

/**
 * @brief Suspend the current fiber and return control to the scheduler.
 *        The fiber will NOT be put back into the ready list.
 * 
 * @param code A value returned by the corresponding fiber_resume.
 * @return FibTCB* The currently running fiber after resumption.
 */
FibTCB * fiber_suspend(uint64_t code);

/**
 * @brief Yield the processor to the next fiber in the ready list.
 *        The current fiber is moved to the end of the ready list.
 * 
 * @param code A value returned by the corresponding fiber_resume.
 * @return FibTCB* The currently running fiber after resumption.
 */
FibTCB * fiber_yield(uint64_t code);

/**
 * @brief Destroy a fiber and free its resources.
 *        Cannot destroy a RUNNING fiber.
 * 
 * @param co The fiber to destroy.
 */
void     fiber_destroy(FibTCB* co);

/**
 * @brief Check the status of a fiber.
 */
FibState fiber_status(FibTCB* co);

/**
 * @brief Get the currently running fiber in the current thread.
 */
FibTCB * fiber_ident();

/**
 * @brief Service thread entry point. 
 *        Loops and yields until only the main fiber remains.
 *        Pass a fiber_scheduler_args_t* to set the idle callback,
 *        or NULL for no callback.
 */
void *   fiber_thread_entry(void* args);

/**
 * @brief Sleep for a specified number of microseconds.
 */
void     fiber_usleep(int64_t usec);



/**
 * @brief Set fiber-local data.
 */
static inline bool fiber_set_localdata(FibTCB * tcb, int index, uint64_t data) {
    if (index >= MAX_FIBER_LOCALDATAS) return false;
    tcb->taskLocalStorages[index] = data;
    return true;
}

/**
 * @brief Get fiber-local data.
 */
static inline uint64_t fiber_get_localdata(FibTCB * tcb, int index) {
    if (index >= MAX_FIBER_LOCALDATAS) return 0ULL;
    return tcb->taskLocalStorages[index];
}

/**
 * @brief Install per-fiber lifecycle callbacks.
 *        onTaskStartup is called before the fiber's entry function.
 *        onTaskCleanup is called after the fiber's entry function returns.
 */
bool fiber_install_callbacks(
    FibTCB * the_task,
    bool (* onTaskStartup)(FibTCB *),
    bool (* onTaskCleanup)(FibTCB *)
);

/////////////////////////////////////////////////////////////////////////
/* Scheduler callback args                                             */
/////////////////////////////////////////////////////////////////////////
typedef struct {
    bool     (*fiberSchedulerCallback)(void *);
    void *   args;
} fibthread_args_t, fiber_scheduler_args_t;
/////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif

#endif // __FIBER_H__
