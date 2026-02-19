#ifndef __LIBFIB_TASK_H__
#define __LIBFIB_TASK_H__

#include "sysdef.h"

#include "chain.h"
#include "spinlock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_GLOBAL_GROUPS           (  16)

#define MAX_LOCAL_FREED_TASKS       ( 128)
#define TCB_INCREASE_SIZE_AT_EMPTY  (  64)

#define MAX_TASK_LOCALDATAS         (   4)

#define FIBER_STACKSIZE_MIN         (8192)
#define FIBER_TIMEOUT_NOWAIT        (   0)
#define FIBER_TIMEOUT_INFINITE      (  -1)

/*
 *  The following constants define the individual states which may be
 *  be used to compose and manipulate a thread's state.
 */
#define STATES_READY           (0x00000) /* ready to run        */
#define STATES_TRANSIENT       (0x00001) /* off the queue       */
#define STATES_SUSPENDED       (0x00002) /* waiting for resume  */
#define STATES_IN_USLEEP       (0x00004) /* waiting for resume  */
#define STATES_WAIT_TIMEOUTB   (0x00008) /* wait for timeout    */
#define STATES_WAITFOR_EVENT   (0x00010) /* wait for event      */
#define STATES_WAITFOR_MUTEX   (0x00020) /* wait for mutex      */
#define STATES_WAITFOR_SEMPH   (0x00040) /* wait for semaphore  */
#define STATES_WAITFOR_CONDV   (0x00080) /* wait for conditional variable */

#define STATES_BLOCKED         (STATES_SUSPENDED     |    \
                                STATES_IN_USLEEP     |    \
                                STATES_WAIT_TIMEOUTB |    \
                                STATES_WAITFOR_EVENT |    \
                                STATES_WAITFOR_MUTEX |    \
                                STATES_WAITFOR_SEMPH |    \
                                STATES_WAITFOR_CONDV )

/* user provied stack */
#define MASK_SYSTEM_STACK      (0x00001)
#define MASK_SUSPEND_AT_ENTRY  (0x00002)

#define TASK_EVENT_WAIT_ANY    (0x00001)

struct FibTCB;
typedef struct FibTCB FibTCB;
typedef struct FibTCB * fiber_t;

struct FibSCP;
typedef struct FibSCP FibSCP;

#if defined(__aarch64__)
typedef struct FibCTX {
    // Callee-saved General Purpose Registers
    uint64_t x19, x20;
    uint64_t x21, x22;
    uint64_t x23, x24;
    uint64_t x25, x26;
    uint64_t x27, x28;
    uint64_t x29; // Frame Pointer (fp)

    // Stack Pointer and Link Register
    uint64_t sp;
    uint64_t lr; // Link Register (x30)
    
    // Callee-saved Floating-Point/SIMD Registers
    uint64_t d8, d9;
    uint64_t d10, d11;
    uint64_t d12, d13;
    uint64_t d14, d15;

    uint64_t _d;
} FibCTX;
#else
typedef struct FibCTX{
    uint64_t    reg_r12;    // 0
    uint64_t    reg_r13;    // 8
    uint64_t    reg_r14;    // 16
    uint64_t    reg_r15;    // 24

    uint64_t    reg_rip;    // 32
    uint64_t    reg_rsp;    // 40
    uint64_t    reg_rbx;    // 48
    uint64_t    reg_rbp;    // 56
} FibCTX;
#endif

typedef CHAIN_HEAD(fibchain, FibTCB) fibchain_t;

struct FibTCB{
    /* ready / suspend / free queue */
    CHAIN_ENTRY(FibTCB) node;  

    /* start point, arguments */
    void *    (*entry)(void *);
    void *    args;

    /* stack address & size */
    void *    stackaddr;
    uint32_t  stacksize;

    /* task state (READY/SUSPEND/SLEEP/WAIT) */
    volatile uint32_t state;

    /* group */
    uint32_t group;
    uint32_t fut32;

    /*  yieldCode / exitCode */
    uint64_t  yieldCode;

    /* timeout support */
    CHAIN_ENTRY(FibTCB) link;
    int32_t    delta_interval;

    /* events waiting/post */
    int32_t    waitingOptions;
    uint64_t   pendingEvents;
    uint64_t   waitingEvents;
    uint64_t   seizedEvents;

    spinlock_t eventlock;

    /* waiting mutex or semaphore */
    uint64_t  waitingObject;

    /* thread message queue associated with this task */
    FibTCB *  scheduler;
    FibSCP *  scheddata;
    void   *  _pd;

    /* Task Local Storage */
    uint64_t  taskLocalStorages[MAX_TASK_LOCALDATAS];

    /* on task initialization & deinit */
    bool      (* onTaskStartup)(FibTCB *);
    bool      (* onTaskCleanup)(FibTCB *);

    /* CPU switching context */
    FibCTX    regs;
};

struct FibSCP {
    FibTCB      *  taskonrun;
    void        *  freedlist;

    fibchain_t     readylist;
    fibchain_t     wadoglist;

    spinlock_t     readylock;
    spinlock_t     wadoglock;
    
    volatile uint32_t nLocalFibTasks;
    uint32_t       cached_stack_mask;

    int32_t        freedsize;
    int32_t        group;

    struct {
        void *   stackbase;
        uint32_t stacksize;
        uint32_t pad32;
    } cached_stack[8];
};

typedef struct FiberMutex {
    volatile uint64_t holder;
    volatile uint32_t reentries;

    spinlock_t qlock;
    fibchain_t waitq;
} FibMutex;

typedef struct FiberSemaphore {
    spinlock_t qlock;
    fibchain_t waitq;

    volatile int64_t count;
} FibSemaphore;

typedef struct FiberCondition {
    spinlock_t qlock;
    fibchain_t waitq;
} FibCondition;

typedef struct FibMsgQ {
    volatile int64_t head;
    volatile int64_t tail;

    int32_t qsize, dsize;

    uint8_t * buffer;
    void (*copyfunc)(void * dest, const void * src);
    FibSemaphore semSpac;
    FibSemaphore semData;
} FibMsgQ;

typedef FibMutex     fiber_mutex_t;
typedef FibSemaphore fiber_sem_t;
typedef FibCondition fiber_cond_t;
typedef FibMsgQ      fiber_msgq_t;

///////////////////////////////////////////////////////////////////
/* coroutine lib standard APIs:                                  */
/* 1. libary initialization                                      */
/* 2. create a task                                              */
/* 3. yield                                                      */
/* 4. resume                                                     */
/* ------------------------------------------------------------- */
/* 5. usleep (task level usleep, wont hangup the thread)         */
/* 6. sched_yield                                                */
///////////////////////////////////////////////////////////////////
/* called @ system startup */
bool FiberGlobalStartup();
bool FiberThreadStartup();

/* create a task */
FibTCB * fiber_create(
    void *(*func)(void *), 
    void * args, 
    void * stackaddr, 
    uint32_t stacksize
    );

/* yield will yield control to other task
 * current task will suspend until resume called on it
 */
FibTCB * fiber_suspend(uint64_t code);
uint64_t fiber_resume(FibTCB * the_task);

/* identify current running task */
FibTCB * fiber_ident();

/* task usleep (accurate at ms level)*/
void fiber_usleep(int usec);

/* same functionality of sched_yield, yield the processor
 * sched_yield causes the calling task to relinquish the CPU.
 * The task is moved to the end of the ready queue and 
 * a new task gets to run.
 */
FibTCB * fiber_yield(uint64_t);

/* service thread (scheduler) entry point*/
void * fiber_thread_entry(void * args);
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
/* service thread maintask related functions                     */
///////////////////////////////////////////////////////////////////
/* wait for events (events bitmask in, wait for any/all, and timeout) 
   post event to a task (timeout in us)
*/ 
uint64_t fiber_event_wait(uint64_t events_in, int options, int timeout);
int      fiber_event_post(FibTCB * the_task, uint64_t events_in);

/* mutex APIs */
#if defined(__M_N_MODEL__)
int  fiber_mutex_init   (FibMutex * pmutex);
bool fiber_mutex_lock   (FibMutex * pmutex);
bool fiber_mutex_unlock (FibMutex * pmutex);
bool fiber_mutex_destroy(FibMutex * pmutex);
#else
#define fiber_mutex_lock(mtx)
#define fiber_mutex_unlock(mtx)
#define fiber_mutex_init(mtx)
#define fiber_mutex_destroy(mtx)
#endif

/* sempahore APIs (timeout in us) */
int  fiber_sem_init(FibSemaphore * psem, int initval);
bool fiber_sem_wait(FibSemaphore * psem);
bool fiber_sem_timedwait(FibSemaphore * psem, int timeout);
bool fiber_sem_post(FibSemaphore * psem);
bool fiber_sem_destroy(FibSemaphore * psem);

/* Conditional Variables (timeout in us) */
int  fiber_cond_init(FibCondition * pcond);
bool fiber_cond_wait(FibCondition * pcond, FibMutex * pmutex);
bool fiber_cond_timedwait(FibCondition * pcond, FibMutex * pmutex, int timeout);
bool fiber_cond_signal(FibCondition * pcond);
bool fiber_cond_broadcast(FibCondition * pcond);
bool fiber_cond_destroy(FibCondition * pcond);
///////////////////////////////////////////////////////////////////

//////////////////////////////////
/* assembly code in context.S   */
//////////////////////////////////
void swap_context(void *, void *);
void asm_taskmain(              );
//////////////////////////////////

typedef struct fiber_scheduler_args_t {
    bool (*fiberSchedulerCallback)(void *);
    void * args;
} fibthread_args_t, fiber_scheduler_args_t;
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
/* extensions                                                    */
///////////////////////////////////////////////////////////////////
static inline bool fiber_set_localdata(FibTCB * the_task, int index, uint64_t data){
    if (index >= MAX_TASK_LOCALDATAS){return false;};
    the_task->taskLocalStorages[index] = data;
    return true;
}

static inline uint64_t fiber_get_localdata(FibTCB * the_task, int index){
    if (index >= MAX_TASK_LOCALDATAS){return 0ULL;};

    return the_task->taskLocalStorages[index];
}

static inline bool fiber_install_callbacks(
    FibTCB * the_task,
    bool (* onTaskStartup)(FibTCB *      ),
    bool (* onTaskCleanup)(FibTCB *      )    
    )
{
    the_task->onTaskStartup       = onTaskStartup;
    the_task->onTaskCleanup       = onTaskCleanup;

    return true;
}
///////////////////////////////////////////////////////////////////

#ifdef __cplusplus
};
#endif

#endif
