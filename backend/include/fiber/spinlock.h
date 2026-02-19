#ifndef _SPINLOCK_CMPXCHG_H
#define _SPINLOCK_CMPXCHG_H

    #if defined(__x86_64__) || defined(__i386__)
        #define cpu_relax() asm volatile("pause\n": : :"memory")
    #elif defined(__aarch64__)
        #define cpu_relax() asm volatile("yield": : :"memory")
    #else
        #define cpu_relax() // No-op on other architectures
    #endif

    #if defined(__M_N_MODEL__)
        #if defined(__SPINLOCK_USING_MUTEX__)
        
        #include <pthread.h>
        typedef pthread_mutex_t spinlock_t;

        #define spin_init(pmutex)  pthread_mutex_init(pmutex, NULL)
        #define spin_lock(pmutex)  pthread_mutex_lock(pmutex)
        #define spin_unlock(pmtx)  pthread_mutex_unlock(pmtx)
        #define spin_destroy(pmtx) pthread_mutex_destroy(pmtx)

        #define __spin_lock(pmtx) spin_lock(pmtx)
        #define __spin_unlock(pmtx) spin_unlock(pmtx)

        #else

        #ifdef __cplusplus
        extern "C" {
        #endif

        // 1. Correctly align to a 64-byte cache line
        typedef struct {
            volatile int32_t lock;
        } __attribute__((aligned(64))) spinlock_t;

        #define SPINLOCK_ATTR static __inline __attribute__((always_inline, no_instrument_function))

        SPINLOCK_ATTR void spin_init(spinlock_t *s) {
            s->lock = 0;
        }

        SPINLOCK_ATTR void spin_destroy(spinlock_t *s) {
            // No-op
        }

        // 2. Implement a Test-and-Test-and-Set (TTAS) spinlock with acquire semantics
        SPINLOCK_ATTR void spin_lock(spinlock_t *s) {
            while (1) {
                // First, spin on a local read until the lock appears free.
                // This is the "Test" part of TTAS.
                // __ATOMIC_RELAXED is fine here since we are not yet in the critical section.
                while (__atomic_load_n(&s->lock, __ATOMIC_RELAXED)) {
                    cpu_relax();
                }

                // Now that the lock might be free, try to acquire it.
                // This is the "Test-and-Set" part.
                // Use __ATOMIC_ACQUIRE to ensure no code from the critical
                // section is reordered before this point.
                if (__atomic_exchange_n(&s->lock, 1, __ATOMIC_ACQUIRE) == 0) {
                    return; // Success
                }
            }
        }

        // 3. Implement unlock with correct release semantics
        SPINLOCK_ATTR void spin_unlock(spinlock_t *s) {
            // Use __ATOMIC_RELEASE to ensure all writes in the critical section
            // are visible before the lock is released.
            __atomic_store_n(&s->lock, 0, __ATOMIC_RELEASE);
        }

        #define __spin_lock(pmtx) spin_lock(pmtx)
        #define __spin_unlock(pmtx) spin_unlock(pmtx)

        #ifdef __cplusplus
        };
        #endif
        #endif
        
    #else

        #ifdef __cplusplus
        extern "C" {
        #endif

        // 1. Correctly align to a 64-byte cache line
        typedef struct {
            volatile int32_t lock;
        } __attribute__((aligned(64))) spinlock_t;

        #define SPINLOCK_ATTR static __inline __attribute__((always_inline, no_instrument_function))

        SPINLOCK_ATTR void spin_init(spinlock_t *s) {
            s->lock = 0;
        }

        SPINLOCK_ATTR void spin_destroy(spinlock_t *s) {
            // No-op
        }

        // 2. Implement a Test-and-Test-and-Set (TTAS) spinlock with acquire semantics
        SPINLOCK_ATTR void __spin_lock(spinlock_t *s) {
            while (1) {
                // First, spin on a local read until the lock appears free.
                // This is the "Test" part of TTAS.
                // __ATOMIC_RELAXED is fine here since we are not yet in the critical section.
                while (__atomic_load_n(&s->lock, __ATOMIC_RELAXED)) {
                    cpu_relax();
                }

                // Now that the lock might be free, try to acquire it.
                // This is the "Test-and-Set" part.
                // Use __ATOMIC_ACQUIRE to ensure no code from the critical
                // section is reordered before this point.
                if (__atomic_exchange_n(&s->lock, 1, __ATOMIC_ACQUIRE) == 0) {
                    return; // Success
                }
            }
        }

        // 3. Implement unlock with correct release semantics
        SPINLOCK_ATTR void __spin_unlock(spinlock_t *s) {
            // Use __ATOMIC_RELEASE to ensure all writes in the critical section
            // are visible before the lock is released.
            __atomic_store_n(&s->lock, 0, __ATOMIC_RELEASE);
        }
        
        #define spin_lock(s)
        #define spin_unlock(s)

        #ifdef __cplusplus
        };
        #endif

    #endif

#endif /* _SPINLOCK_CMPXCHG_H */
