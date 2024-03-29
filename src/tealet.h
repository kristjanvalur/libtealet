/********** A minimal coroutine package for C **********/
#ifndef _TEALET_H_
#define _TEALET_H_

#include <stddef.h>

#define TEALET_VERSION "0.1.0"

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


/* A structure to define the memory allocation api used.
 * the functions have C89 semantics and take an additional "context"
 * pointer that they can use as they please
 */
typedef void*(*tealet_malloc_t)(size_t size, void *context);
typedef void(*tealet_free_t)(void *ptr, void *context);
typedef struct tealet_alloc_t {
  tealet_malloc_t malloc_p;
  tealet_free_t free_p;
  void *context;
} tealet_alloc_t;

/* use the following macro to initialize a tealet_alloc_t
 * structure with stdlib malloc functions, for convenience, e.g.:
 * tealet_alloc_t stdalloc = TEALET_MALLOC;
 */
#define TEALET_ALLOC_INIT_MALLOC {\
    (tealet_malloc_t)&malloc, \
    (tealet_free_t)&free, \
    0 \
}

/* convenence macros to call an allocator */
#define TEALET_ALLOC_MALLOC(alloc, size) (alloc)->malloc_p((size), (alloc)->context)
#define TEALET_ALLOC_FREE(alloc, ptr) (alloc)->free_p((ptr), (alloc)->context)


/* The user-visible tealet structure.  If an "extrasize" is provided when
 * the main tealet was initialized, "extra" points to a private block of
 * that size, otherwise it is initialized to NULL
 */
typedef struct tealet_t {
  struct tealet_t *main;   /* pointer to the main tealet */
  void *extra;
  /* private fields follow */
} tealet_t;

/* The "run" function of a tealet.  It is called with the
 * current tealet and the argument provided to its invocation function, 
 * see tealet_create() and tealet_switch().
 * The return value of run() must be the next tealet in which to continue
 * execution, which must be a different one, like for example the main tealet.
 * When 'run(g)' returns, the tealet 'g' is freed.
 */
typedef tealet_t *(*tealet_run_t)(tealet_t *current, void *arg);

/* error codes.  API functions that return int return a negative value
 * to signal an error.
 * Those that return tealet_t pointers return NULL to signal a memory
 * error.
 */
#define TEALET_ERR_MEM -1       /* memory allocation failed */
#define TEALET_ERR_DEFUNCT -2   /* the target tealet is corrupt */

/* Initialize and return the main tealet.  The main tealet contains the whole
 * "normal" execution of the program; it starts when the program starts and
 * ends when the program ends.  This function and tealet_finalize() should
 * be called together from the same (main) function which calls the rest of
 * the program.  It is fine to nest several uses of initialize/finalize,
 * or to call them in multiple threads in case of multithreaded programs,
 * as long as you don't try to switch between tealets created with a
 * different main tealet.
 * If 'extrasize' is non-zero, all tealets will be allocated with an
 * extra data buffer of 'extrasize' and their 'extra' member initialized
 * to point to that buffer.  Otherwise, the 'extra' members are set
 * to NULL.
 */
TEALET_API
tealet_t *tealet_initialize(tealet_alloc_t *alloc, size_t extrasize);

/* Tear down the main tealet.  Call e.g. after a thread finishes (including
 * all its tealets).
 */
TEALET_API
void tealet_finalize(tealet_t *tealet);

/* access to the tealet's allocator.  This can be useful to use
 * e.g. when passing data between tealets but such data cannot
 * reside on the stack (except during the initial call to tealet_new)
 */
TEALET_API
void *tealet_malloc(tealet_t *tealet, size_t s);
TEALET_API
void tealet_free(tealet_t *tealet, void *p);

/* Allocate a new tealet 'g' with a callback 'run'.
 * The tealet can subsequently be run by calling
 * tealet_switch(g, arg) which will invoke 'run(g, *arg)' in it.
 * The return value of run() must be the next tealet in which to continue
 * execution, which must be a different one, like for example the main tealet.
 * When 'run(g)' returns, the tealet 'g' is freed.
 * The return value is the new tealet, or NULL if memory allocation failed.
 * Note that this tealet may have been alread freed should run(g) have
 * returned by the time this function returns.
 * On return, *arg contains the arg value passed in to the switch
 * causing this return.
 * 'arg' can be NULL, in which case NULL is passed to run and no result
 * argument is passed.
 */
TEALET_API
tealet_t *tealet_create(tealet_t *tealet, tealet_run_t run);

/* Switch to another tealet.  Execution continues there.  The tealet
 * passed in must not have been freed yet and must descend from
 * the same main tealet as the current one.  In multithreaded applications,
 * it must also belong to the current thread (otherwise, segfaults).
 * if 'arg' is non-NULL, the argument passed in *arg will be provided
 * to the other tealet when it returns from its tealet_new() or 
 * tealet_switch().
 * On return, *arg contains whatever *arg was
 * provided when switching back here.
 * Take care to not have *arg point to stack allocated data because
 * such data may be overwritten when the context switches.
 */
TEALET_API
int tealet_switch(tealet_t *target, void **parg);

/* Exit the current tealet and switch to a target.
 * Similar to tealet_switch except that never returns
 * unless the TEALET_FLAG_DEFER flag is set.
 * In case the desired target is defunct, it will switch to the
 * main tealet instead.
 * This allows passing of an arg to the target tealet, in addition to
 * controlling whether the exiting tealet is automatically deleted or not.
 * This is the recommended way to exit a tealet, because it is symmetric
 * to the entry point, allowing us to pass back a value.
 * This function can be used as an emergency measure to return to the
 * main tealet if tealet_switch() fails due to inability to save the stack.
 * Note that exiting to the main tealet is always guaranteed to work.
 * Returning with 'p' from the tealet's run function is equivalent to calling
 * tealet_exit(p, NULL, TEALET_FLAG_DELETE), but the explicit way is
 * recommended.
 * If the TEALET_FLAG_DEFER flag is set, then this function merely sets the
 * flag and arg values.  It returns 0, and the calling function can proceed to
 * return with 'p' from its run() function.  This is useful if a clean "return" is desired,
 * for example to call destructors.
 */
#define TEALET_FLAG_NONE 0
#define TEALET_FLAG_DELETE 1
#define TEALET_FLAG_DEFER 2
TEALET_API
int tealet_exit(tealet_t *target, void *arg, int flags);

/* Allocate and switch to a new tealet.
 * This is semantically equvalent to
 * tealet_create() followed by tealet_switch(), but may be slightly faster.
 * The return value is the new tealet, or NULL if memory allocation failed.
 * Note that this tealet may have been alread freed should run(g) have
 * returned by the time this function returns.
 */
TEALET_API
tealet_t *tealet_new(tealet_t *tealet, tealet_run_t run, void **parg);

/* Duplicate a tealet. The active tealet is duplicated
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

/* Deallocate a tealet.  Use this to delete a tealet that has exited
 * with tealet_exit() with 'TEALET_EXIT_NODELETE', or defunct tealets.
 * Active tealet can also be
 * deleted, such as stubs that are no longer in use, but take care
 * because any local resources in such tealets won't be freed.
 */
TEALET_API
void tealet_delete(tealet_t *target);

/* Return the current tealet, i.e. the one in which the caller of this
 * function currently is.  "tealet" can be any tealet derived from the
 * main tealet.
 */
TEALET_API
tealet_t *tealet_current(tealet_t *tealet);

/* Return the previous tealet, i.e. the one that switched to us.
* "tealet" can be any tealet derived from the
 * main tealet.
 */
TEALET_API
tealet_t *tealet_previous(tealet_t *tealet);

/* Get the address of a the tealet's main user pointer, a single
 * void pointer associated with the main tealet.  Use this to
 * associate a (void*)value with the main tealet.
 */
TEALET_API
void **tealet_main_userpointer(tealet_t *tealet);

/* functions for stack arithmetic.  Useful when deciding
 * if you want to start a newa tealet, or when doing stack
 * spilling
 */

/* subtract two stack positions, taking into account if the
 * local stack grows up or down in memory.
 * The result is positive if 'b' is 'deeper' on the stack
 * than 'a'
 */
TEALET_API
ptrdiff_t tealet_stack_diff(void *a, void *b);

/* Get a tealet's "far" position on the stack.  This is an
 * indicator of its creation position on the stack.  The main
 * tealet extends until the beginning of stack
 */
TEALET_API
void *tealet_get_far(tealet_t *tealet);

/* this is used to get the "far address _if_ a tealet were initialized here
 * The arguments must match the real tealet_new() but are dummies.
 */
TEALET_API
void *tealet_new_far(tealet_t *dummy1, tealet_run_t dummy2, void **dummy3);

/* get the size of the suspended tealet's saved stack */
TEALET_API
size_t tealet_get_stacksize(tealet_t *tealet);

/* get a tealet's status */
#define TEALET_STATUS_ACTIVE 0
#define TEALET_STATUS_EXITED 1
#define TEALET_STATUS_DEFUNCT -2
TEALET_API
int tealet_status(tealet_t *tealet);

/* get status about the tealets. */
typedef struct tealet_stats_t
{
    int n_active;
    int n_total;
} tealet_stats_t;
TEALET_API
void tealet_get_stats(tealet_t *t, tealet_stats_t *s);

/* Convenience macros */
#define TEALET_MAIN(t) ((t)->main)
#define TEALET_IS_MAIN(t)  ((t) == TEALET_MAIN(t))
#define TEALET_CURRENT_IS_MAIN(t) (tealet_current(t) == TEALET_MAIN(t))

/* see if two tealets share the same MAIN, and can therefore be switched between */
#define TEALET_RELATED(t1, t2) (TEALET_MAIN(t1) == TEALET_MAIN(t2))

/* convenience access to a typecast extra pointer */
#define TEALET_EXTRA(t, tp) ((tp*)((t)->extra))

#endif /* _TEALET_H_ */
