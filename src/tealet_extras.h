/**
 * @file tealet_extras.h
 * @brief Public helper extensions for libtealet.
 */
#include "tealet.h"
#ifndef _TEALET_EXTRAS_H_
#define _TEALET_EXTRAS_H_

/****************************************************************
 * A tealet allocator that gathers usage statistics
 */

typedef struct tealet_statsalloc_t {
  tealet_alloc_t alloc;
  tealet_alloc_t *base;
  size_t n_allocs;
  size_t s_allocs;
} tealet_statsalloc_t;

TEALET_API
void tealet_statsalloc_init(tealet_statsalloc_t *alloc, tealet_alloc_t *base);

/****************************************************************
 * Convenience creation wrappers built on tealet_new() + tealet_run().
 */

/* Allocate/bind without immediate switch; equivalent to tealet_run(..., TEALET_RUN_DEFAULT). */
TEALET_API
int tealet_prepare(tealet_t *tealet, tealet_t **pcreated, tealet_run_t run, void *stack_far);

/* Allocate/bind and immediately switch; equivalent to tealet_run(..., TEALET_RUN_SWITCH). */
TEALET_API
int tealet_spawn(tealet_t *tealet, tealet_t **pcreated, tealet_run_t run, void **parg, void *stack_far);

/****************************************************************
 * A tealet stub mechanism.
 * A stub is a special paused tealet, that can be restarted to
 * run any function.  It can also be duplicated, providing a
 * convenient mechanism to start a family of tealets from a common
 * position on the stack.
 */

/* create a stub and return it via out-parameter */
TEALET_API
int tealet_stub_new(tealet_t *tealet, tealet_t **pstub, void *stack_far);

/*
 * Run a previously created stub.
 * Behaviour is similar to tealet_spawn(), except that 'stub' must be the
 * result of tealet_stub_new(), or the result
 * of tealet_duplicate() on such a stub.  Otherwise
 * behaviour is undefined.
 */
TEALET_API
int tealet_stub_run(tealet_t *stub, tealet_run_t run, void **parg);

#endif /* _TEALET_EXTRAS_H_ */
