#ifndef TEALETEX_SETCONTEXT_H
#define TEALETEX_SETCONTEXT_H

/*
 * Draft setcontext-like API built on top of libtealet.
 *
 * This header is intentionally example-oriented: it sketches an interface
 * similar to getcontext/makecontext/swapcontext/setcontext while keeping
 * libtealet naming and data ownership explicit.
 */

#include "tealet.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Explicit setcontext domain rooted in one libtealet main tealet.
 *
 * This draft intentionally avoids a hidden static global so callers control
 * lifetime, thread affinity, and teardown order explicitly.
 */
typedef struct tealetex_setcontext_main_t {
  tealet_t *main;
} tealetex_setcontext_main_t;

/* Context lifecycle state bits for tealetex_ucontext_t::uc_state. */
#define TEALETEX_UCSTATE_EMPTY (0u)
#define TEALETEX_UCSTATE_BOUND (1u << 0)
#define TEALETEX_UCSTATE_ACTIVE (1u << 1)
#define TEALETEX_UCSTATE_EXITED (1u << 2)

/* Maximum number of makecontext arguments supported by this example API. */
#define TEALETEX_MAKECONTEXT_MAX_ARGS (4)

/* Canonical makecontext-style callback signature. */
typedef void (*tealetex_context_func_t)();

/*
 * setcontext-style context object.
 *
 * - uc_tealet is the underlying tealet handle when bound.
 * - uc_main identifies the libtealet domain this context belongs to.
 * - uc_link is the continuation used when the entry function returns,
 *   analogous to ucontext uc_link.
 */
typedef struct tealetex_ucontext_t {
  tealet_t *uc_tealet;
  tealet_t *uc_main;
  struct tealetex_ucontext_t *uc_link;

  tealetex_context_func_t uc_func;
  int uc_argc;
  uintptr_t uc_argv[TEALETEX_MAKECONTEXT_MAX_ARGS];

  unsigned int uc_state;
} tealetex_ucontext_t;

/*
 * Initialize/finalize a setcontext domain.
 *
 * init allocates the underlying libtealet main tealet (malloc allocator,
 * extrasize 0), sets its far boundary for fork-based getcontext capture,
 * and initializes @p scmain. fini finalizes it and clears the handle.
 */
int tealetex_getcontext_init(tealetex_setcontext_main_t *scmain, void *far_boundary);
void tealetex_getcontext_fini(tealetex_setcontext_main_t *scmain);

/*
 * Initialize @p ucp as a context descriptor in @p scmain's domain.
 *
 * Similar intent to getcontext(): prepare a descriptor tied to a calling
 * domain. This does not bind a runnable entry yet.
 */
int tealetex_getcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp);

/*
 * Bind a runnable entry to @p ucp.
 *
 * Similar intent to makecontext(). @p argc arguments are captured from varargs
 * into @p ucp and dispatched by arity when the context first starts.
 *
 * This example implementation supports up to TEALETEX_MAKECONTEXT_MAX_ARGS
 * arguments (each passed/stored as uintptr_t).
 */
int tealetex_makecontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp, tealetex_context_func_t func,
                         int argc, ...);

/*
 * Swap from @p oucp to @p ucp, exchanging optional argument via @p parg.
 *
 * Similar intent to swapcontext(). On success, returns 0 after control later
 * switches back into @p oucp.
 */
int tealetex_swapcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *oucp, tealetex_ucontext_t *ucp,
                         void **parg);

/*
 * Transfer to @p ucp without saving return continuation in a separate output
 * context.
 *
 * Similar intent to setcontext(). Success path transfers control and does not
 * return to the caller until another context explicitly switches back.
 */
int tealetex_setcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp, void **parg);

/*
 * Release resources associated with @p ucp (if bound).
 *
 * This is an explicit cleanup helper for the example API; it maps to
 * tealet_delete() for non-main underlying contexts.
 */
void tealetex_freecontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp);

#ifdef __cplusplus
}
#endif

#endif /* TEALETEX_SETCONTEXT_H */
