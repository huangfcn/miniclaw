/* Single-threaded OpenMP API stub.
 *
 * Used on platforms that have no OpenMP runtime (iOS): faiss includes
 * <omp.h> unconditionally, so this header satisfies those includes while
 * every #pragma omp in the source compiles to a plain sequential loop
 * (clang ignores unknown pragmas). All queries report a single thread and
 * the locks are no-ops — correct because nothing runs concurrently.
 *
 * Search performance at miniclaw's memory scale (<= ~100k vectors) is not
 * meaningfully affected by single-threading.
 */
#ifndef MC_OMP_STUB_H
#define MC_OMP_STUB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int omp_lock_t;

static inline int  omp_get_max_threads(void)  { return 1; }
static inline int  omp_get_num_threads(void)  { return 1; }
static inline int  omp_get_thread_num(void)   { return 0; }
static inline int  omp_in_parallel(void)      { return 0; }
static inline void omp_set_num_threads(int n) { (void)n; }
static inline int  omp_get_nested(void)       { return 0; }
static inline void omp_set_nested(int n)      { (void)n; }

static inline int  omp_init_lock(omp_lock_t* l)   { *l = 0; return 0; }
static inline void omp_destroy_lock(omp_lock_t* l){ *l = 0; }
static inline void omp_set_lock(omp_lock_t* l)    { (void)l; }
static inline void omp_unset_lock(omp_lock_t* l)  { (void)l; }

#ifdef __cplusplus
}
#endif

#endif /* MC_OMP_STUB_H */
