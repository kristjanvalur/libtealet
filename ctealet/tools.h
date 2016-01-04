#include "tealet.h"
#ifndef _TEALET_TOOLS_H_
#define _TEALET_TOOLS_H_

/****************************************************************
 * A tealet allocator that gathers usage statistics
 */


typedef struct tealet_statsalloc_t
{
    tealet_alloc_t alloc;
    tealet_alloc_t *base;
    size_t n_allocs;
    size_t s_allocs;
} tealet_statsalloc_t;

TEALET_API
void
tealet_statsalloc_init(tealet_statsalloc_t *alloc, tealet_alloc_t *base);


/****************************************************************
 * A tealet stub mechanism.
 * A stub is a special paused tealet, that can be restarted to
 * run any function.  It can also be duplicated, providing a
 * convenient mechanism to start a family of tealets from a common
 * position on the stack.
 */

 /* create a stub and return it */
TEALET_API
tealet_t *
tealet_stub_new(tealet_t *tealet);

/*
 * Run a previously created stub.
 * Behaviour is similar to tealet_new(), except that 'stub' must be the
 * result of tealet_stub_new(), or the result
 * of tealet_duplicate() on such a stub.  Otherwise
 * behaviour is undefined.
 */
TEALET_API
int
tealet_stub_run(tealet_t *stub, tealet_run_t run, void **parg);


#endif /* _TEALET_TOOLS_H_ */