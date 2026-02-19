#include <stdint.h>
#include <stdbool.h>

#ifndef __TENGINE_SYSDEF_H__
#define __TENGINE_SYSDEF_H__
#define EPSILON       (1e-10)

///////////////////////////////////////////////////////////////////////////////
/* malloc / free / new / delete                                              */
///////////////////////////////////////////////////////////////////////////////
#ifndef _WIN32
#define _aligned_malloc(n, a) aligned_alloc(a, n)
#define _aligned_free(p)      free(p)
#endif
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
/* min/max/abs                                                               */
///////////////////////////////////////////////////////////////////////////////
#define _dabs(f)              (((f) < 0) ? (-(f)) : (f))
#define _dmin(a, b)           (((a) < (b)) ? (a)  : (b))
#define _dmax(a, b)           (((a) > (b)) ? (a)  : (b))
///////////////////////////////////////////////////////////////////////////////

typedef double       double_t;
typedef const char * lpcstr_t;

///////////////////////////////////////////////////////////////////////////////
/* snprintf/sprintf                                                          */
///////////////////////////////////////////////////////////////////////////////
// #include "stb_sprintf.h"
// #define sprintf        stb_sprintf
// #define snprintf       stb_snprintf
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
/* GNU C++ specific likely/unlikely                                          */
///////////////////////////////////////////////////////////////////////////////
#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
/* GNU C++ specific hot attribute                                            */
///////////////////////////////////////////////////////////////////////////////
#ifdef __GNUC__
#define HOTSPOT         __attribute__ ((hot))
#else
#define HOTSPOT
#endif
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
/* GNU C++ specific noinline                                                 */
///////////////////////////////////////////////////////////////////////////////
#ifdef __GNUC__
#define __force_noinline __attribute__ ((noinline)) 
#define __forceinline    __inline __attribute__((always_inline, no_instrument_function))
#else
#define __force_noinline __declspec(noinline)
#endif
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

    #include <intrin.h>

    ///////////////////////////////////////////////////////////////////////////////
    /* lockfree CAS                                                              */
    ///////////////////////////////////////////////////////////////////////////////
    #define CAS64(ptr, oldval, newval)  (_InterlockedCompareExchange64 ((ptr), (newval), (oldval)) == (oldval))
    #define CAS32(ptr, oldval, newval)  (_InterlockedCompareExchange   ((ptr), (newval), (oldval)) == (oldval))
    #define CAS2( ptr,   oldp,   newp)  (_InterlockedCompareExchange128((ptr), (newp)[1], (newp)[0], (oldp)))

    #define FAA(ptr)                    (_InterlockedIncrement64(ptr))
    #define FAS(ptr)                    (_InterlockedDecrement64(ptr))

    #define CACHE_ALIGN_PRE             __declspec(align(64))
    #define CACHE_ALIGN_POST
    ///////////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////////
    /* BYTE SWAP                                                                 */
    ///////////////////////////////////////////////////////////////////////////////
    #include <stdlib.h>
    #define bswap_16(x) (_byteswap_ushort((x)))
    #define bswap_32(x) (_byteswap_ulong ((x)))
    #define bswap_64(x) (_byteswap_uint64((x)))
    ///////////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////////
    /* thread-specific                                                           */
    ///////////////////////////////////////////////////////////////////////////////
    #define qthread_api_t  DWORD WINAPI 
    #define qthread_t      DWORD
    #define qthread_create(tid, start, lpparam) \
        CreateThread(                           \
            NULL, 0L, (start),                  \
            (void *)(lpparam), 0L, &(tid)       \
            )

    #define __thread_local  __declspec(thread)
    ///////////////////////////////////////////////////////////////////////////////

    #ifdef __cplusplus
    extern "C" {
    #endif
    ///////////////////////////////////////////////////////////////////////////
    /* builtin equvalent                                                     */
    ///////////////////////////////////////////////////////////////////////////
    static inline int __builtin_ctz64(uint64_t x){
        unsigned long ret; _BitScanForward64(&ret, x); return (int)ret;
    }
    static inline int __builtin_ctz(uint32_t x){
        unsigned long ret; _BitScanForward(&ret, x); return (int)ret;
    }
    static inline int __builtin_clz64(uint64_t x){
        return __lzcnt64(x);
    }
    static inline int __builtin_clz(uint32_t x){
        return __lzcnt(x);
    }
    ///////////////////////////////////////////////////////////////////////////
        static inline int sched_yield()
        {
            SwitchToThread();
            return (0);
        };

        static inline int sleep(int secs){
            Sleep(secs * 1000);
            return (0);
        }

        static inline void usleep(__int64 usec)
        {
            HANDLE timer;
            LARGE_INTEGER ft;

            ft.QuadPart = -(10 * usec);

            timer = CreateWaitableTimer(NULL, TRUE, NULL);
            SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        };
        
        static inline void _nanosleep(__int64 nsec)
        {
            HANDLE timer; LARGE_INTEGER ft;

            ft.QuadPart = -((nsec < 100) ? (1) : (ns / 100));

            timer = CreateWaitableTimer(NULL, TRUE, NULL);
            SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        };

    #ifdef __cplusplus
    };
    #endif

#else  // !_WIN32

    #ifdef __cplusplus
    extern "C" {
    #endif

    #if defined(__x86_64__) || defined(__aarch64__)
        // Use __int128_t and compiler built-ins for a portable and safe 128-bit CAS.
        // This works for both x86-64 (which compiles to lock cmpxchg16b) and
        // AArch64 (which compiles to a ldxp/stxp loop).
        static inline char CAS2(volatile int64_t* addr, int64_t* oldval, const int64_t* newval)
        {
            // Note: The 'oldval' parameter for this function must be non-volatile
            // because __atomic_compare_exchange_n needs to be able to write to it
            // on failure. The caller must ensure this is safe.
            __int128_t old_val_128 = ((__int128_t)oldval[1] << 64) | (uint64_t)oldval[0];
            __int128_t new_val_128 = ((__int128_t)newval[1] << 64) | (uint64_t)newval[0];

            // The compiler will generate the correct atomic instruction for the target arch.
            return __atomic_compare_exchange_n(
                (volatile __int128_t*)addr,
                &old_val_128,
                new_val_128,
                0, // Not a weak CAS
                __ATOMIC_SEQ_CST,
                __ATOMIC_RELAXED
            );
        }
    #endif

    #ifdef __cplusplus
    };
    #endif

    ///////////////////////////////////////////////////////////////////////////////
    /* lockfree CAS                                                              */
    ///////////////////////////////////////////////////////////////////////////////
    /*
    * Modern Atomic Operation Macros using __atomic built-ins.
    * These are generally faster than the legacy __sync built-ins because they
    * allow for specifying a more relaxed memory ordering.
    */

    // --- Compare-And-Swap (CAS) ---
    // For CAS, SEQ_CST is often the safest default, as it's frequently used
    // for complex synchronization where strict ordering is required.
    // The failure ordering can be relaxed.
    #define CAS32(ptr, oldval, newval) \
        __atomic_compare_exchange_n(ptr, &(oldval), newval, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)

    #define CAS64(ptr, oldval, newval) \
        __atomic_compare_exchange_n(ptr, &(oldval), newval, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)


    // --- Fetch-And-Add / Fetch-And-Sub ---
    // These are often used for simple counters where ordering is not important.
    // Using __ATOMIC_RELAXED provides a significant performance boost.
    #define FAA(ptr) __atomic_fetch_add((ptr), 1, __ATOMIC_RELAXED)
    #define FAS(ptr) __atomic_fetch_sub((ptr), 1, __ATOMIC_RELAXED)


    #define __ffs32(ptr)                __builtin_ffs(ptr)
    #define __ffs64(ptr)                __builtin_ffsll(ptr)

    #define CACHE_ALIGN_PRE
    #define CACHE_ALIGN_POST            __attribute__ ((aligned (64)))
    ///////////////////////////////////////////////////////////////////////////////

    #include <unistd.h>
    #include <sched.h>

    ///////////////////////////////////////////////////////////////////////////////
    /* BYTE SWAP                                                                 */
    ///////////////////////////////////////////////////////////////////////////////
    #ifdef _MSC_VER
    #include <stdlib.h>
    #define bswap_16(x) _byteswap_ushort(x)  
    #define bswap_32(x) _byteswap_ulong(x)
    #define bswap_64(x) _byteswap_uint64(x)
    #elif defined(__APPLE__)
    // Mac OS X / Darwin features
    #include <libkern/OSByteOrder.h>
    #define bswap_16(x) OSSwapInt16(x)
    #define bswap_32(x) OSSwapInt32(x)
    #define bswap_64(x) OSSwapInt64(x)
    #else
    #include <byteswap.h>
    #endif
    ///////////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////////
    /* x86intrin                                                                 */
    ///////////////////////////////////////////////////////////////////////////////
    #if defined(__x86_64__) || defined(__i386__)
    #include <x86intrin.h>
    #endif
    ///////////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////////
    /* thread-specific                                                           */
    ///////////////////////////////////////////////////////////////////////////////
    #include <pthread.h>
    #define qthread_api_t  void *
    #define qthread_t      pthread_t
    #define qthread_create(id, start, lpparam) \
        pthread_create(                        \
            &(id), NULL,                       \
            (start), (void *)(lpparam)         \
            )
    #define __thread_local  __thread
    ///////////////////////////////////////////////////////////////////////////////

    #define _nanosleep(ns) {struct timespec req = {0, ns}; nanosleep(&req, NULL);}
#endif // _WIN32

#endif
