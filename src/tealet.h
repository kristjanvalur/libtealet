/**
 * @file tealet.h
 * @brief Public core API for libtealet.
 */
/* A minimal coroutine package for C */
#ifndef _TEALET_H_
#define _TEALET_H_

#include <stddef.h>

/* Version information */
#define TEALET_VERSION_MAJOR 0
#define TEALET_VERSION_MINOR 4
#define TEALET_VERSION_PATCH 3

/* Version as a string */
#define TEALET_VERSION "0.4.3"

/* Version as a single number for comparisons (MMmmpp: Major, minor, patch) */
#define TEALET_VERSION_NUMBER ((TEALET_VERSION_MAJOR * 10000) + (TEALET_VERSION_MINOR * 100) + TEALET_VERSION_PATCH)

#ifdef WIN32
#if defined TEALET_EXPORTS
#define TEALET_API __declspec(dllexport)
#elif defined TEALET_IMPORTS
#define TEALET_API __declspec(dllimport)
#else
#define TEALET_API
#endif
#else /* win32 */
#define TEALET_API
#endif

/** A structure to define the memory allocation api used.
 * the functions have C89 semantics and take an additional "context"
 * pointer that they can use as they please
 *
 * Threading contract with optional tealet locking:
 * allocator callbacks may be invoked either with or without the configured
 * tealet lock held. Integrations must not rely on tealet_lock() for allocator
 * protection, and allocator implementations must avoid deadlocking in either
 * call context.
 */
typedef void *(*tealet_malloc_t)(size_t size, void *context);
typedef void (*tealet_free_t)(void *ptr, void *context);
typedef struct tealet_alloc_t {
  tealet_malloc_t malloc_p;
  tealet_free_t free_p;
  void *context;
} tealet_alloc_t;

/** Optional locking hooks for thread-safe tealet-structure access.
 *
 * The callbacks are invoked by tealet_lock()/tealet_unlock() with @p arg.
 * If either callback is NULL, the corresponding API becomes a no-op.
 *
 * Primary multi-threaded use case: coordinating foreign-thread
 * tealet_delete() of non-main tealets. Configuration, switching, and
 * tealet_duplicate() are intended to remain on one owning thread.
 */
typedef struct tealet_lock_t {
  void (*lock)(void *arg);
  void (*unlock)(void *arg);
  void *arg;
} tealet_lock_t;

/** use the following macro to initialize a tealet_alloc_t
 * structure with stdlib malloc functions, for convenience, e.g.:
 * tealet_alloc_t stdalloc = TEALET_MALLOC;
 */
#define TEALET_ALLOC_INIT_MALLOC                                                                                       \
  { (tealet_malloc_t) & malloc, (tealet_free_t)&free, 0 }

/** convenience macros to call an allocator */
#define TEALET_ALLOC_MALLOC(alloc, size) (alloc)->malloc_p((size), (alloc)->context)
#define TEALET_ALLOC_FREE(alloc, ptr) (alloc)->free_p((ptr), (alloc)->context)

/** The user-visible tealet structure.  If an "extrasize" is provided when
 * the main tealet was initialized, "extra" points to a private block of
 * that size, otherwise it is initialized to NULL
 */
typedef struct tealet_t {
  struct tealet_t *main; /* pointer to the main tealet */
  void *extra;
  /* private fields follow */
} tealet_t;

/** The "run" function of a tealet.  It is called with the
 * current tealet and the argument provided to its invocation function,
 * see tealet_create() and tealet_switch().
 * The return value of run() must be the next tealet in which to continue
 * execution, which must be a different one, like for example the main tealet.
 * When 'run(g)' returns, the tealet 'g' is freed.
 */
typedef tealet_t *(*tealet_run_t)(tealet_t *current, void *arg);

/** error codes.  API functions that return int return a negative value
 * to signal an error.
 * Those that return tealet_t pointers return NULL to signal a memory
 * error.
 */
#define TEALET_ERR_MEM -1     /* memory allocation failed */
#define TEALET_ERR_DEFUNCT -2 /* the target tealet is corrupt */
#define TEALET_ERR_UNFORKABLE                                                                                          \
  -3                            /* tealet cannot be forked (unbounded stack)                                           \
                                 */
#define TEALET_ERR_INVAL -4     /* invalid argument */
#define TEALET_ERR_INTEGRITY -5 /* current tealet violated stack-integrity boundary */
#define TEALET_ERR_PANIC -6     /* switched to main due to panic reroute from tealet_exit() */

/* configuration API structure versioning */
#define TEALET_CONFIG_VERSION_1 1
#define TEALET_CONFIG_CURRENT_VERSION TEALET_CONFIG_VERSION_1

/* stack integrity configuration flags */
#define TEALET_CONFIGF_STACK_INTEGRITY (1u << 0)
#define TEALET_CONFIGF_STACK_GUARD (1u << 1)
#define TEALET_CONFIGF_STACK_SNAPSHOT (1u << 2)

/* stack guard modes */
#define TEALET_STACK_GUARD_MODE_NONE 0
#define TEALET_STACK_GUARD_MODE_READONLY 1
#define TEALET_STACK_GUARD_MODE_NOACCESS 2

/* stack integrity failure policy */
#define TEALET_STACK_INTEGRITY_FAIL_ASSERT 0
#define TEALET_STACK_INTEGRITY_FAIL_ERROR 1
#define TEALET_STACK_INTEGRITY_FAIL_ABORT 2

/* conservative default upper bound for caller stack distance checks */
#define TEALET_DEFAULT_MAX_STACK_SIZE ((size_t)(16u * 1024u * 1024u))

/** Runtime configuration for stack integrity and related safety features.
 *
 * ABI compatibility contract:
 * - 'size' must be the first member and set by caller to
 * sizeof(tealet_config_t) (or a future/older struct size).
 * - 'version' is a struct format version, independent from the global libtealet
 * ABI.
 * - tealet_configure_get()/set() read/write only the prefix known to this
 * build, bounded by 'size'. Unknown tail fields are ignored.
 */
typedef struct tealet_config_t {
  size_t size;
  unsigned int version;
  unsigned int flags;
  size_t stack_integrity_bytes;
  int stack_guard_mode;
  int stack_integrity_fail_policy;
  void *stack_guard_limit;
  size_t max_stack_size; /* max caller stack distance for sanity checks; 0 disables */
  unsigned int reserved[2];
} tealet_config_t;

/* Convenience initializer for configuration structs */
#define TEALET_CONFIG_INIT                                                                                             \
  {                                                                                                                    \
    sizeof(tealet_config_t), TEALET_CONFIG_CURRENT_VERSION, 0u, 0, TEALET_STACK_GUARD_MODE_NONE,                       \
        TEALET_STACK_INTEGRITY_FAIL_ASSERT, NULL, TEALET_DEFAULT_MAX_STACK_SIZE, {                                     \
      0u, 0u                                                                                                           \
    }                                                                                                                  \
  }

/* ----------------------------------------------------------------
 * Public API - core lifecycle and switching
 */

/**
 * @brief Initialize libtealet and create the main tealet for the current thread.
 * @param alloc Allocator interface; use #TEALET_ALLOC_INIT_MALLOC for stdlib malloc/free.
 * @param extrasize Optional per-tealet extra bytes; exposed via tealet_t::extra / #TEALET_EXTRA.
 * @return Pointer to the main tealet, or NULL on allocation failure.
 *
 * The main tealet represents the ambient program execution context for this thread.
 * Initialize/finalize pairs may be nested or used in multiple threads, but only
 * tealets derived from the same main tealet are switch-compatible.
 */
TEALET_API
tealet_t *tealet_initialize(tealet_alloc_t *alloc, size_t extrasize);

/**
 * @brief Destroy a previously initialized main tealet.
 * @param tealet Main tealet returned by tealet_initialize().
 *
 * @warning This does not delete child tealets. Delete all non-main tealets first.
 * @warning After finalize returns, all handles associated with that main tealet are invalid.
 */
TEALET_API
void tealet_finalize(tealet_t *tealet);

/**
 * @brief Create a new tealet without starting it.
 * @param tealet Main/related tealet context used for allocation and ownership.
 * @param run Entry function for the created tealet.
 * @param stack_far Optional minimum far-boundary requirement for the initial stack snapshot.
 * @return New tealet pointer, or NULL on allocation/setup failure.
 *
 * The new tealet enters execution when tealet_switch() first targets it.
 * If @p stack_far is non-NULL, capture range is only extended (never shrunk)
 * relative to the default internally selected boundary.
 */
TEALET_API
tealet_t *tealet_create(tealet_t *tealet, tealet_run_t run, void *stack_far);

/**
 * @brief Create and immediately start a new tealet.
 * @param tealet Main/related tealet context.
 * @param run Entry function.
 * @param parg In/out switch argument pointer (same semantics as tealet_switch()).
 * @param stack_far Optional minimum far-boundary requirement for the initial stack snapshot.
 * @return New tealet pointer on success; NULL on allocation/switch failure.
 *
 * Semantically equivalent to tealet_create() followed by tealet_switch(),
 * but performed as one operation.
 */
TEALET_API
tealet_t *tealet_new(tealet_t *tealet, tealet_run_t run, void **parg, void *stack_far);

/**
 * @brief Suspend current tealet and resume @p target.
 * @param target Tealet to switch to; must share the same main tealet and thread.
 * @param parg In/out argument pointer passed across switches; may be NULL.
 * @retval 0 Success.
 * @retval TEALET_ERR_MEM Save/restore failed due to memory pressure.
 * @retval TEALET_ERR_DEFUNCT Target tealet/stack is defunct.
 * @retval TEALET_ERR_PANIC Returned to main after panic reroute from tealet_exit().
 * @retval TEALET_ERR_INVAL Invalid state/target.
 *
 * @warning Do not pass stack-allocated cross-tealet payloads through @p parg.
 */
TEALET_API
int tealet_switch(tealet_t *target, void **parg);

/* Exit flags */
#define TEALET_EXIT_DEFAULT 0 /* Don't auto-delete */
#define TEALET_EXIT_DELETE 1  /* Auto-delete on exit */
#define TEALET_EXIT_DEFER 2   /* Defer exit to return statement */

/**
 * @brief Exit current tealet and transfer control to @p target.
 * @param target Requested target tealet.
 * @param arg Optional argument to deliver to @p target.
 * @param flags Exit behavior bits: #TEALET_EXIT_DEFAULT, #TEALET_EXIT_DELETE, #TEALET_EXIT_DEFER.
 * @return 0 only for deferred setup path; otherwise this call is non-returning on success.
 *
 * If the requested target is defunct, control is rerouted to main as panic fallback.
 * `return p;` from run() is equivalent to `tealet_exit(p, NULL, TEALET_EXIT_DELETE)`.
 */
TEALET_API
int tealet_exit(tealet_t *target, void *arg, int flags);

/**
 * @brief Fork the active tealet by duplicating its execution state.
 * @param current Currently active tealet to duplicate.
 * @param pother Optional out-pointer to the opposite side (parent gets child, child gets parent).
 * @param parg Optional in/out argument pointer passed to whichever side resumes later.
 * @param flags Fork mode: #TEALET_FORK_DEFAULT or #TEALET_FORK_SWITCH.
 * @retval 1 Parent side.
 * @retval 0 Child side.
 * @retval TEALET_ERR_UNFORKABLE Current stack is unbounded (set far boundary first).
 * @retval TEALET_ERR_MEM Memory allocation failure.
 *
 * @warning For forks originating from main-lineage execution (main tealet or a clone
 * of main), exit explicitly via tealet_exit() and do not use #TEALET_EXIT_DEFER.

 * This function duplicates the currently executing tealet, including its entire
 * execution stack. Both the parent and child tealets will resume execution from
 * the same point (immediately after tealet_fork returns).
 *
 * This function duplicates the currently executing tealet, including its entire
 * execution stack. Both the parent and child tealets will resume execution from
 * the same point (immediately after tealet_fork returns).
 *
 * The function returns different values to distinguish parent from child:
 * - In the parent tealet: returns 1
 * - In the child tealet: returns 0
 * - On error: returns -1 (or other negative TEALET_ERR_* code)
 *
 * If pchild is non-NULL, it will be filled with a pointer to the other tealet:
 * - In the parent: points to the newly created child tealet
 * - In the child: points to the parent tealet
 * This allows both parent and child to reference each other for switching.
 *
 * The parg parameter allows passing a pointer value to whichever side of the
 * fork initially gets suspended (similar to tealet_new()). Can be NULL if no
 * argument passing is desired. If non-NULL:
 * - With TEALET_FORK_DEFAULT: The parent continues, child is suspended.
 *   When the parent later switches to the child, the child receives the
 *   value via *parg.
 * - With TEALET_FORK_SWITCH: The parent is suspended, child continues.
 *   When the child later switches back, the parent receives the value
 *   via *parg.
 * See tealet_new() documentation for more details on argument passing
 * semantics.
 *
 * Prerequisites:
 * - The current tealet must be either:
 *   a) A regular function-scoped tealet, OR
 *   b) The main tealet (or a clone of main) with a bounded stack
 *      (far boundary set via tealet_set_far()).
 * - The current tealet must be active (not suspended)
 *
 * Flags:
 * - TEALET_FORK_DEFAULT (0): Child is created suspended; parent continues
 * - TEALET_FORK_SWITCH (1): Immediately switch to child after creation
 *
 * Memory considerations:
 * - The child tealet has its own stack copy (heap-allocated)
 * - Both parent and child share the same main tealet
 * - The child inherits the current tealet far boundary (`stack_far`)
 *   at fork time. For main-lineage forks, this means the configured
 *   main boundary propagates to the child clone.
 * - Stack-allocated data is duplicated, heap data is shared
 * - Use tealet_current() to get the current tealet pointer after forking
 *
 * Exit behavior depends on what was forked:
 * - Forking main-lineage execution (main tealet or a clone of main) creates a
 *   continuation without a normal run-function return path. In that case,
 *   exit via tealet_exit() with an explicit target and do not use
 *   TEALET_EXIT_DEFER.
 * - Forking a regular function-scoped tealet duplicates that existing call
 *   chain. The forked tealet can generally return through the same run-function
 *   path as the original tealet.
 *
 * Example:
 *   tealet_t *child = NULL;
 *   int result = tealet_fork(current, &child, NULL, TEALET_FORK_DEFAULT);
 *   if (result == 0) {
 *       // This is the child tealet
 *       // ... child-specific code ...
 *   } else if (result > 0) {
 *       // This is the parent tealet
 *       // ... parent-specific code ...
 *       tealet_switch(child, &arg); // Switch to child when ready
 *   } else {
 *       // Error occurred (result < 0)
 *   }
 *
 * Returns:
 *   Parent: 1
 *   Child: 0
 *   Error: negative error code:
 *     TEALET_ERR_UNFORKABLE if current tealet has unbounded stack
 *     TEALET_ERR_MEM if memory allocation failed
 *     TEALET_ERR_DEFUNCT if current tealet is not active
 */
#define TEALET_FORK_DEFAULT 0
#define TEALET_FORK_SWITCH 1
TEALET_API
int tealet_fork(tealet_t *current, tealet_t **pother, void **parg, int flags);

/**
 * @brief Duplicate a suspended tealet and its saved stack state.
 * @param tealet Source tealet (must not be current/main).
 * @return Duplicated tealet, or NULL on failure.
 *
 * Duplicate starts with copied stack snapshot and copied internal flags.
 * Extra payload area (`extra`) is copied when configured.
 *
 * Duplicate a tealet. The active tealet is duplicated
 * along with its stack contents.
 * This can be used, for example, to create "stubs" that can be duplicated
 * and re-used to run with different arguments.
 * Use this with care, because initially the duplicate will share the same
 * stack data as the original.  This includes any local variables, even the
 * "current" argument passed to the original "run" function which may
 * be obsolete by the time the duplicate is run.  Use the argument passing
 * mechanism to provide the copy with fresh data.
 * Any extra data is left uncopied, the application must do that.
 */
TEALET_API
tealet_t *tealet_duplicate(tealet_t *tealet);

/**
 * @brief Deallocate a non-main tealet.
 * @param target Tealet to delete.
 *
 * Deallocate a tealet.  Use this to delete a tealet that has exited
 * with tealet_exit() with 'TEALET_EXIT_DEFAULT', or defunct tealets.
 * Active tealet can also be
 * deleted, such as stubs that are no longer in use, but take care
 * because any local resources in such tealets won't be freed.
 */
TEALET_API
void tealet_delete(tealet_t *target);

/* ----------------------------------------------------------------
 * Public API - status and query
 */

/** Return the current tealet, i.e. the one in which the caller of this
 * function currently is.  "tealet" can be any tealet derived from the
 * main tealet.
 */
TEALET_API
tealet_t *tealet_current(tealet_t *tealet);

/** Return the previous tealet, i.e. the one that switched to us.
 * "tealet" can be any tealet derived from the
 * main tealet.
 *
 * Query APIs in this section are not internally synchronized. If they are
 * called from foreign threads, or concurrently with lifecycle/switching
 * operations, callers must provide external synchronization (for example
 * tealet_lock()/tealet_unlock() around the entire access sequence).
 *
 * Note: in a multithreaded setting, a previous tealet can be simultaneously
 * deleted by a different thread, invalidating the returned pointer.
 */
TEALET_API
tealet_t *tealet_previous(tealet_t *tealet);

/** Get the address of the tealet's main user pointer, a single
 * void pointer associated with the main tealet.  Use this to
 * associate a (void*)value with the main tealet.
 */
TEALET_API
void **tealet_main_userpointer(tealet_t *tealet);

/* Tealet origin flags returned by tealet_get_origin(). */
#define TEALET_ORIGIN_MAIN_LINEAGE (1u << 0) /* main tealet, or fork-descended from main */
#define TEALET_ORIGIN_FORK (1u << 1)         /* tealet originated from tealet_fork() */

/**
 * @brief Get tealet origin flags.
 * @param tealet Target tealet.
 * @return Bitwise OR of #TEALET_ORIGIN_* flags.
 *
 * Origin flags are immutable identity markers:
 * - #TEALET_ORIGIN_MAIN_LINEAGE marks main-like lineage (main and descendants
 *   produced through tealet_fork() from main lineage).
 * - #TEALET_ORIGIN_FORK marks tealets that originated from tealet_fork().
 */
TEALET_API
unsigned int tealet_get_origin(tealet_t *tealet);

/* Status code: active tealet. */
#define TEALET_STATUS_ACTIVE 0
/* Status code: exited tealet. */
#define TEALET_STATUS_EXITED 1
/* Status code: defunct tealet. */
#define TEALET_STATUS_DEFUNCT -2
TEALET_API
int tealet_status(tealet_t *tealet);

/**
 * @brief Get byte size of currently saved stack snapshot for a tealet.
 * @param tealet Target tealet.
 * @return Saved stack bytes, or 0 when active/defunct/no saved stack.
 */
TEALET_API
size_t tealet_get_stacksize(tealet_t *tealet);

/**
 * @brief Get a tealet's far stack boundary marker.
 * @param tealet Target tealet.
 * @return Far boundary pointer.
 *
 * Get a tealet's "far" position on the stack.  This is an
 * indicator of its creation position on the stack.  The main
 * tealet extends until the beginning of stack
 */
TEALET_API
void *tealet_get_far(tealet_t *tealet);

/* Aggregate resource statistics for a main-tealet domain. */
typedef struct tealet_stats_t {
  /* Basic tealet counts */
  int n_active; /* number of active tealets (excluding main) */
  int n_total;  /* total tealets created (cumulative) */

  /* Memory usage statistics */
  size_t bytes_allocated;        /* Current heap allocation */
  size_t bytes_allocated_peak;   /* Peak heap allocation  */
  size_t blocks_allocated;       /* Current number of allocated stack blocks */
  size_t blocks_allocated_peak;  /* Peak number of allocated stack blocks */
  size_t blocks_allocated_total; /* Total allocation calls */

  /* stack memory storage statistics */
  size_t stack_bytes;          /* Bytes used for stack storage */
  size_t stack_bytes_expanded; /* Bytes used for stack if there were no reuse */
  size_t stack_bytes_naive;    /* Bytes used for stack if we stored stack naively */
  size_t stack_count;          /* Number of currently stored unique stacks */
  size_t stack_chunk_count;    /* Number of currently stored unique stack chunks */
} tealet_stats_t;

TEALET_API
void tealet_get_stats(tealet_t *t, tealet_stats_t *s);

TEALET_API
void tealet_reset_peak_stats(tealet_t *t);

/* ----------------------------------------------------------------
 * Public API - configuration
 */

/**
 * Set the far boundary of a tealet's stack.
 *
 * This function sets the far boundary of a tealet's stack, which defines where
 * the stack ends for stack-slicing operations.
 *
 * For the main tealet: The main tealet normally has an unbounded stack extent
 * (represented internally as STACKMAN_SP_FURTHEST). To enable operations like
 * tealet_fork() that need to duplicate the stack, you must first set a far
 * boundary.
 *
 * IMPORTANT: The far_boundary pointer should typically come from a PARENT
 * function of the function that will perform fork operations. This ensures that
 * all local variables in the forking function (and any functions it calls) are
 * included in the saved stack slice. If you pass a local variable from the same
 * function that calls fork, that variable and others declared after it may not
 * be properly saved.
 *
 * Recommended pattern (far_boundary from parent function):
 *
 *   int main(void) {
 *       int far_marker;
 *       run_program(&far_marker);
 *       return 0;
 *   }
 *
 *   void run_program(void *far_marker) {
 *       tealet_t *main = tealet_initialize(&alloc, 0);
 *       tealet_set_far(main, far_marker);
 *
 *       int local_var = 0;
 *       tealet_fork(main, &child, 0);
 *   }
 *
 * Alternative (far_boundary from same function, requires care):
 *
 *   void my_main() {
 *       int far_marker;
 *       tealet_t *main = tealet_initialize(&alloc, 0);
 *       tealet_set_far(main, &far_marker);
 *
 *       int local_var = 0;
 *       tealet_fork(main, &child, 0);
 *   }
 *
 * By providing this address, you promise that no stack data beyond (further
 * from) this point needs to be saved during fork/duplicate operations.
 *
 * Note: Currently, this function can only be called on the main tealet. Calling
 * it on a non-main tealet will return an error.
 *
 * Returns:
 *   0 on success
 *   -1 if called from a non-main tealet
 */
TEALET_API
int tealet_set_far(tealet_t *tealet, void *far_boundary);

/**
 * @brief Get effective runtime configuration for a main tealet.
 * @param tealet Any tealet in the domain.
 * @param config Size/versioned output buffer.
 * @return 0 on success, negative #TEALET_ERR_* on failure.
 *
 * Get or set runtime configuration on the main tealet.
 *
 * These functions are intended for feature toggles such as optional stack
 * integrity checks. The caller supplies a size/versioned config struct.
 *
 * tealet_configure_set() canonicalizes the struct in-place to the effective
 * configuration actually applied by this build/platform (unsupported features
 * are cleared and dependent fields normalized).
 *
 * Return values:
 *   0 on success
 *  <0 on error (e.g. TEALET_ERR_INVAL)
 */
TEALET_API
int tealet_configure_get(tealet_t *tealet, tealet_config_t *config);

/**
 * @brief Set runtime configuration for a main tealet.
 * @param tealet Any tealet in the domain.
 * @param config Size/versioned input/output configuration struct.
 * @return 0 on success, negative #TEALET_ERR_* on failure.
 *
 * Input is canonicalized in-place to effective applied settings.
 */
TEALET_API
int tealet_configure_set(tealet_t *tealet, tealet_config_t *config);

/**
 * @brief Configure optional locking callbacks for a main-tealet domain.
 * @param tealet Any tealet in the domain.
 * @param locking Lock callback descriptor to copy; pass NULL to clear (no-op mode).
 * @return 0 on success.
 *
 * This stores callback pointers and argument on the domain's main tealet.
 * It does not acquire or release locks itself.
 * Configuration, switching, and tealet_duplicate() remain single-thread
 * responsibilities.
 *
 * Locking/allocator interaction contract: libtealet may call allocator
 * callbacks with or without this lock held, depending on call path.
 * Do not assume either state.
 */
TEALET_API
int tealet_config_set_locking(tealet_t *tealet, const tealet_lock_t *locking);

/**
 * @brief Enable stack-integrity checking with practical defaults.
 * @param tealet Any tealet in the domain.
 * @param stack_integrity_bytes Requested watch window bytes; 0 picks sensible default.
 * @return 0 on success, negative #TEALET_ERR_* on failure.
 *
 * Convenience helper to enable stack checking with sensible defaults.
 *
 * This enables stack integrity checks with both guard pages and snapshot
 * verification, then applies the resulting configuration via
 * tealet_configure_set().
 *
 * If stack_integrity_bytes is 0, a default window of one OS memory page is
 * used where available.
 *
 * Note: this helper sets stack_guard_limit to a local stack marker inside this
 * function. For best results, call it from a program top-level function whose
 * frame should remain within the intended stack region for switched tealets.
 *
 * This helper is intended as a one-way "turn checks on" API. You can still
 * use tealet_configure_set() directly for custom tuning.
 *
 * Return values:
 *   0 on success
 *  <0 on error (e.g. TEALET_ERR_INVAL, TEALET_ERR_MEM)
 */
TEALET_API
int tealet_configure_check_stack(tealet_t *tealet, size_t stack_integrity_bytes);

/* ----------------------------------------------------------------
 * Public API - utility helpers
 */

/**
 * @brief Allocate memory using the tealet-domain allocator.
 * @param tealet Any tealet in the domain.
 * @param s Byte count.
 * @return Allocated block or NULL.
 *
 * access to the tealet's allocator.  This can be useful to use
 * e.g. when passing data between tealets but such data cannot
 * reside on the stack (except during the initial call to tealet_new)
 */
TEALET_API
void *tealet_malloc(tealet_t *tealet, size_t s);

/**
 * @brief Free memory using the tealet-domain allocator.
 * @param tealet Any tealet in the domain.
 * @param p Pointer previously allocated by tealet_malloc() or equivalent domain allocator.
 */
TEALET_API
void tealet_free(tealet_t *tealet, void *p);

/**
 * @brief Invoke configured lock callback for this tealet domain.
 * @param tealet Any tealet in the domain.
 *
 * No-op when no lock callback is configured.
 */
TEALET_API
void tealet_lock(tealet_t *tealet);

/**
 * @brief Invoke configured unlock callback for this tealet domain.
 * @param tealet Any tealet in the domain.
 *
 * No-op when no unlock callback is configured.
 */
TEALET_API
void tealet_unlock(tealet_t *tealet);

/* functions for stack arithmetic.  Useful when deciding
 * if you want to start a new tealet, or when doing stack
 * spilling
 */

/**
 * @brief Direction-aware stack pointer subtraction.
 * @param a First stack position.
 * @param b Second stack position.
 * @return Positive when @p b is deeper than @p a in stack-growth direction.
 *
 * subtract two stack positions, taking into account if the
 * local stack grows up or down in memory.
 * The result is positive if 'b' is 'deeper' on the stack
 * than 'a'
 */
TEALET_API
ptrdiff_t tealet_stack_diff(void *a, void *b);

/**
 * @brief Return whichever of two addresses is farther in stack-growth direction.
 * @param a First stack position.
 * @param b Second stack position.
 * @return Direction-aware farther boundary.
 *
 * Return whichever stack position is farther from the active stack top.
 * This is direction-aware: on descending stacks this is the larger
 * address; on ascending stacks it is the smaller address.
 */
TEALET_API
void *tealet_stack_further(void *a, void *b);

/**
 * @brief Probe helper returning the effective initial far boundary at call site depth.
 * @param dummy1 Matches tealet_new() signature; ignored.
 * @param dummy2 Matches tealet_new() signature; ignored.
 * @param dummy3 Matches tealet_new() signature; ignored.
 * @param dummy4 Optional requested boundary (as in tealet_new()).
 * @return Effective far boundary that tealet_new() would use at this stack depth.
 *
 * this is used to get the "far" address if a tealet were initialized here.
 * The arguments must match tealet_new(); they are only dummies.
 */
TEALET_API
void *tealet_new_probe(tealet_t *dummy1, tealet_run_t dummy2, void **dummy3, void *dummy4);

/* Convenience macros */
#define TEALET_MAIN(t) ((t)->main)
#define TEALET_IS_MAIN(t) ((t) == TEALET_MAIN(t))
#define TEALET_CURRENT_IS_MAIN(t) (tealet_current(t) == TEALET_MAIN(t))
#define TEALET_IS_MAIN_LINEAGE(t) ((tealet_get_origin(t) & TEALET_ORIGIN_MAIN_LINEAGE) != 0)
#define TEALET_IS_FORK(t) ((tealet_get_origin(t) & TEALET_ORIGIN_FORK) != 0)

/* see if two tealets share the same MAIN, and can therefore be switched between
 */
#define TEALET_RELATED(t1, t2) (TEALET_MAIN(t1) == TEALET_MAIN(t2))

/* convenience access to a typecast extra pointer */
#define TEALET_EXTRA(t, tp) ((tp *)((t)->extra))

#endif /* _TEALET_H_ */
