/********** A minimal coroutine package for C **********
 * By Armin Rigo
 * Documentation: see the source code of the greenlet package from
 *
 *     http://codespeak.net/svn/greenlet/trunk/c/_greenlet.c
 */

#include "tealet.h"
#include <stackman.h>

#include <stddef.h>
#if !defined _MSC_VER || _MSC_VER >= 1600 /* VS2010 and above */
#include <stdint.h> /* for intptr_t */
#endif
#include <assert.h>
#include <string.h>


/* enable collection of tealet stats */
#ifndef TEALET_WITH_STATS
#define TEALET_WITH_STATS 1
#endif


/************************************************************/

/* #define DEBUG_DUMP */

#ifdef DEBUG_DUMP
#include <stdio.h>
#endif

/************************************************************
 * Structures for maintaining copies of the C stack.
 */

/* a chunk represents a single segment of saved stack */
typedef struct tealet_chunk_t
{
    struct tealet_chunk_t *next;   /* additional chunks */
    char *stack_near;              /* near stack address */
    size_t size;                   /* amount of data saved */
    char data[1];                  /* the data follows here */
} tealet_chunk_t;

/* The main stack structure, contains the initial chunk and a link to further
 * segments.  Stacks can be shared by different tealets, hence the reference
 * count.  They can also be linked into a list of partially unsaved
 * stacks, that are saved only on demand.
 */
typedef struct tealet_stack_t
{
    int refcount;                   /* controls lifetime */
    struct tealet_stack_t **prev;   /* previous 'next' pointer */
    struct tealet_stack_t *next;	/* next unsaved stack */
    char *stack_far;                /* the far boundary of this stack */
    size_t saved;                   /* total amount of memory saved in all chunks */
    struct tealet_chunk_t chunk;    /* the initial chunk */
} tealet_stack_t;


/* the actual tealet structure as used internally 
 * The main tealet will have stack_far set to the end of memory.
 * "stack" is zero for a running tealet, otherwise it points
 * to the saved stack, or is -1 if the state is invalid.
 * In addition, stack_far is set to NULL value to indicate
 * that a tealet is exiting.
 */
typedef struct tealet_sub_t {
  tealet_t base;				   /* the public part of the tealet */
  char *stack_far;                 /* the "far" end of the stack or NULL when exiting */
  tealet_stack_t *stack;           /* saved stack or 0 if active or -1 if invalid*/
#ifndef NDEBUG
  int id;                          /* number of this tealet */
#endif
} tealet_sub_t;

/* a structure incorporating extra data */
typedef struct tealet_nonmain_t {
  tealet_sub_t base;
  double _extra[1];                /* start of any extra data */
} tealet_nonmain_t;

/* an enum to maintain state for the save/restore callback
 * which is called twice (with old and new stack pointer)
 */
typedef enum tealet_sr_e
{
    SW_NOP,     /* do nothing (no-restore stack) */
    SW_RESTORE, /* restore stack */
    SW_ERR,     /* error occurred when saving */
} tealet_sr_e;

/* The main tealet has additional fields for housekeeping */
typedef struct tealet_main_t {
  tealet_sub_t base;
  void         *g_user;     /* user data pointer for main */
  tealet_sub_t *g_current;
  tealet_sub_t *g_previous;
  tealet_sub_t *g_target;   /* Temporary store when switching */
  void         *g_arg;      /* argument passed around when switching */
  tealet_alloc_t g_alloc;   /* the allocation context used */
  tealet_stack_t *g_prev;   /* previously active unsaved stacks */
  tealet_sr_e   g_sw;       /* save/restore state */
#if TEALET_WITH_STATS
  int g_tealets;            /* number of active tealets excluding main */
  int g_counter;            /* total number of tealets */
#endif
  size_t       g_extrasize; /* amount of extra memory in tealets */
  double _extra[1];         /* start of any extra data */
} tealet_main_t;

#define TEALET_IS_MAIN_STACK(t)  (((tealet_sub_t *)(t))->stack_far == STACKMAN_SP_FURTHEST)
#define TEALET_GET_MAIN(t)     ((tealet_main_t *)(((tealet_t *)(t))->main))

/************************************************************
 * helpers to call the malloc functions provided by the user
 */
static void *tealet_int_malloc(tealet_main_t *main, size_t size)
{
    return main->g_alloc.malloc_p(size, main->g_alloc.context);
}
static void tealet_int_free(tealet_main_t *main, void *ptr)
{
    main->g_alloc.free_p(ptr, main->g_alloc.context);
}

/*************************************************************
 * actual stack management routines.  Copying, growing
 * restoring, duplicating, deleting
 */
static tealet_stack_t *tealet_stack_new(tealet_main_t *main,
    char *stack_near, char *stack_far, size_t size)
{
    size_t tsize;
    tealet_stack_t *s;
    
    tsize = offsetof(tealet_stack_t, chunk.data[0]) + size;
    s = (tealet_stack_t*)tealet_int_malloc(main, tsize);
    if (!s)
        return NULL;
    s->refcount = 1;
    s->prev = NULL;
    s->stack_far = stack_far;
    s->saved = size;

    s->chunk.next = NULL;
    s->chunk.stack_near = stack_near;
    s->chunk.size = size;
#if STACK_DIRECTION == 0
    memcpy(&s->chunk.data[0], stack_near, size);
#else
    memcpy(&s->chunk.data[0], stack_near-size, size);
#endif
    return s;
}

static int tealet_stack_grow(tealet_main_t *main,
    tealet_stack_t *stack, size_t size)
{
    tealet_chunk_t *chunk;
    size_t tsize, diff;
    assert(size > stack->saved);

    diff = size - stack->saved;
    tsize = offsetof(tealet_chunk_t, data[0]) + diff;
    chunk = (tealet_chunk_t*)tealet_int_malloc(main, tsize);
    if (!chunk)
        return TEALET_ERR_MEM;
#if STACK_DIRECTION == 0
    chunk->stack_near = stack->chunk.stack_near + stack->saved;
    memcpy(&chunk->data[0], chunk->stack_near, diff);
#else
    chunk->stack_near = stack->chunk.stack_near - stack->saved;
    memcpy(&chunk->data[0], chunk->stack_near - diff, diff);
#endif
    chunk->size = diff;
    chunk->next = stack->chunk.next;
    stack->chunk.next = chunk;
    stack->saved = size;
    return 0;
}

static void tealet_stack_restore(tealet_stack_t *stack)
{
    tealet_chunk_t *chunk = &stack->chunk;
    do {
#if STACK_DIRECTION == 0
        memcpy(chunk->stack_near, &chunk->data[0], chunk->size);
#else
        memcpy(chunk->stack_near - chunk->size, &chunk->data[0], chunk->size);
#endif
        chunk = chunk->next;
    } while(chunk);
}

static tealet_stack_t *tealet_stack_dup(tealet_stack_t *stack)
{
    stack->refcount += 1;
    return stack;
}

static void tealet_stack_link(tealet_stack_t *stack, tealet_stack_t **head)
{
    assert(stack->prev == NULL);
    assert(*head != stack);
    if (*head)
        assert((*head)->prev == head);
    stack->next = *head;
    if (stack->next)
        stack->next->prev = &stack->next;
    stack->prev = head;
    *head = stack;
}

static void tealet_stack_unlink(tealet_stack_t *stack)
{
    tealet_stack_t *next = stack->next;
    assert(stack->prev);
    assert(*stack->prev == stack);
    if (next)
        assert(next->prev == &stack->next);

    if (next)
        next->prev = stack->prev;
    *stack->prev = next;
    stack->prev = NULL;
}

static void tealet_stack_decref(tealet_main_t *main, tealet_stack_t *stack)
{
    tealet_chunk_t *chunk;
    if (stack == NULL || --stack->refcount > 0)
        return;
    if (stack->prev)
        tealet_stack_unlink(stack);
 
    chunk = stack->chunk.next;
    tealet_int_free(main, (void*)stack);
    while(chunk) {
        tealet_chunk_t *next = chunk->next;
        tealet_int_free(main, (void*)chunk);
        chunk = next;
    }
}

static void tealet_stack_defunct(tealet_main_t *main, tealet_stack_t *stack)
{
    /* stack couldn't be grown.  Release any extra chunks and mark stack as defunct */
    tealet_chunk_t *chunk;
    chunk = stack->chunk.next;
    stack->chunk.next = NULL;
    stack->saved = (size_t)-1;
    while(chunk) {
        tealet_chunk_t *next = chunk->next;
        tealet_int_free(main, (void*)chunk);
        chunk = next;
    }
}

static size_t tealet_stack_getsize(tealet_stack_t *stack)
{
    if (stack->saved != (size_t)-1)
        return stack->saved;
    return 0;
}


/***************************************************************
 * utility functions for allocating and growing stacks
 */

/* save a new stack, at least up to "saveto" */
static tealet_stack_t *tealet_stack_saveto(tealet_main_t *main,
    char *stack_near, char *stack_far, char *saveto, int *full)
{
    ptrdiff_t size;
    if (STACKMAN_SP_LE(stack_far, saveto)) {
        saveto = stack_far;
        *full = 1;
    } else
        *full = 0;
    assert(saveto != STACKMAN_SP_FURTHEST); /* can't save all of memory */
    size = STACKMAN_SP_DIFF(saveto, stack_near);
    if (size < 0)
        size = 0;
    return tealet_stack_new(main, (char*) stack_near, stack_far, size);
}

static int tealet_stack_growto(tealet_main_t *main, tealet_stack_t *stack, char* saveto,
    int *full, int fail_ok)
{
    /* Save more of g's stack into the heap -- at least up to 'saveto'

       g->stack_stop |________|
                     |        |
                     |    __ stop       .........
                     |        |    ==>  :       :
                     |________|         :_______:
                     |        |         |       |
                     |        |         |       |
      g->stack_start |        |         |_______| g->stack_copy

     */
    ptrdiff_t size, saved=(ptrdiff_t)stack->saved;
    int fail;

    /* We shouldn't be completely saved already */
    if (stack->stack_far != STACKMAN_SP_FURTHEST)
        assert(STACKMAN_SP_DIFF(stack->stack_far, stack->chunk.stack_near) > saved);
 
    /* truncate the "stop" */
    if (STACKMAN_SP_LE(stack->stack_far, saveto)) {
        saveto = stack->stack_far;
        *full = 1;
    } else
        *full = 0;
    
    /* total saved size expected after this */
    assert(saveto != STACKMAN_SP_FURTHEST); /* can't save them all */
    size = STACKMAN_SP_DIFF(saveto, stack->chunk.stack_near);
    if (size <= saved)
        return 0; /* nothing to do */
    
    fail = tealet_stack_grow(main, stack, size);
    if (fail == 0)
        return 0;

    if (fail_ok)
        return fail; /* caller can deal with failures */
    /* we cannot fail.  Mark this stack as defunct and continue */
    tealet_stack_defunct(main, stack);
    *full = 1;
    return 0;
}

/* Grow a list of stacks to a certain limit.  Unlink those that
 * become fully saved.
 */
static int tealet_stack_grow_list(tealet_main_t *main, tealet_stack_t *list, 
    char *saveto, tealet_stack_t *target, int fail_ok)
{
    while (list) {
        int fail;
        int full;
        if (list == target) {
            /* this is the stack we are switching to.  We should stop here 
             * since previous stacks are already fully saved wrt. this.
             * also, if this stack is not shared, it need not be saved
             */
            if (list->refcount > 1) {
                /* saving because the target stack is shared.  If failure cannot
                 * be handled, the target will be marked invalid on error. But
                 * the caller of this function will have already checked that
                 * and will complete the switch despite such a flag.  Only
                 * subsequent uses of this stack will fail.
                 */
                fail = tealet_stack_growto(main, list, saveto, &full, fail_ok);
                if (fail)
                    return fail;
                if (fail_ok)
                    assert(full); /* we saved it entirely */
            }
            tealet_stack_unlink(list);
            return 0;
        }
                
        fail = tealet_stack_growto(main, list, saveto, &full, fail_ok);
        if (fail)
            return fail;
        if (full)
            tealet_stack_unlink(list);
        list = list->next;
    }
    return 0;
}

/*********************************************************************
 * the save and restore callbacks.  These implement all the stack
 * save and restore logic using previously defined functions
 */

/* main->g_target contains the tealet we are switching to:
 * target->stack_far is the limit to which we must save the old stack
 * target->stack can be NULL, indicating that the target stack
 * needs not be restored.
 */
static int tealet_save_state(tealet_main_t *g_main, void *old_stack_pointer)
{
    tealet_sub_t *g_target = g_main->g_target;
    tealet_sub_t *g_current = g_main->g_current;
    char* target_stop = g_target->stack_far;
    int exiting, fail, fail_ok;
    assert(target_stop != NULL); /* target is't exiting */
    assert(g_current != g_target);
    
    exiting = g_current->stack_far == NULL;
    /* if task is exiting, failure cannot be signalled. the switch
     * must proceed. A stack failing to get saved (due to memory shortage)
     * with this flag set will be set as invalid, so that it cannot
     * be switched back to again.
     */
    fail_ok = !exiting;

    /* save and unlink older stacks on demand */
    /* when coming from main, there should be no list of unsaved stacks */
    if (TEALET_IS_MAIN_STACK(g_main->g_current)) {
        assert(!exiting);
        assert(g_main->g_prev == NULL);
    }
    fail = tealet_stack_grow_list(g_main, g_main->g_prev, target_stop, g_target->stack, fail_ok);
    if (fail)
        return -1;
    /* when returning to main, there should now be no list of unsaved stacks */
    if (TEALET_IS_MAIN_STACK(g_main->g_target))
        assert(g_main->g_prev == NULL);
    
    if (exiting) {
        /* tealet is exiting. We don't save its stack. */
        assert(!TEALET_IS_MAIN_STACK(g_current));
        if (g_current->stack == NULL) {
            /* auto-delete the tealet */
#if TEALET_WITH_STATS
            g_main->g_tealets--;
#endif
            tealet_int_free(g_main, g_current);
        } else {
            /* -1 means do-not-delete */
            assert(g_current->stack == (tealet_stack_t*)-1);
            g_current->stack = NULL;
        }
    } else {
        /* save the initial stack chunk */
        int full;
        tealet_stack_t *stack = tealet_stack_saveto(g_main, (char*) old_stack_pointer,
            g_current->stack_far, target_stop, &full);
        if (!stack) {
            if (fail_ok)
                return -1;
            assert(!TEALET_IS_MAIN_STACK(g_current));
            g_current->stack = (tealet_stack_t *)-1; /* signal invalid stack */
        } else {
            g_current->stack = stack;
            /* if it is partially saved, link it in to previous stacks */
            if (TEALET_IS_MAIN_STACK(g_current))
                assert(!full); /* always link in the main tealet's stack */
            if (!full)
                tealet_stack_link(stack, &g_main->g_prev);
        }
    }
    return 0;
}

static void tealet_restore_state(tealet_main_t *g_main, void *new_stack_pointer)
{
    tealet_sub_t *g = g_main->g_target;

    /* Restore the heap copy back into the C stack */
    assert(g->stack != NULL);
    tealet_stack_restore(g->stack);
    tealet_stack_decref(g_main, g->stack);
    g->stack = NULL;
}

/* this callback is called twice from the raw switch code, once after saving
 * registers, where it should save the stack (if needed) and once after
 * updating the stack pointer, where it should restore the stack (if needed)
 */
static void *tealet_save_restore_cb(void *context, int opcode, void *stack_pointer)
{
    tealet_main_t *g_main = (tealet_main_t *)context;
    tealet_sub_t *g_target = g_main->g_target;
        
    if (opcode == STACKMAN_OP_SAVE) {
        int result = tealet_save_state(g_main, stack_pointer);
        if (result) {
            g_main->g_sw = SW_ERR;
            return stack_pointer;
        }
        if (g_target->stack == NULL) {
            /* save only, no restore, keep stack pointer */
            g_main->g_sw = SW_NOP;
            return stack_pointer;
        } else {
            /* return new stack pointer and flag us for restore */
            g_main->g_sw = SW_RESTORE;
            return g_target->stack->chunk.stack_near;
        }
    }
    assert(opcode == STACKMAN_OP_RESTORE);
    if (g_main->g_sw == SW_RESTORE) {
        tealet_restore_state(g_main, stack_pointer);
        return NULL;
    } else if (g_main->g_sw == SW_ERR) {
        /* called second time, but error happened the first time */
        return (void*) -1;
    } 
    /* called second time but no restore should happen */
    assert(g_main->g_sw == SW_NOP);
    return NULL;
}

static int tealet_switchstack(tealet_main_t *g_main)
{
    /* note: we can't pass g_target simply as an argument here, because
     of the mix between different call stacks: after tealet_switch() it
     might end up with a different value.  But g_main is safe, because
     it should have always the same value before and after the switch. */
    tealet_sub_t *previous = g_main->g_previous;
    assert(g_main->g_target);
    assert(g_main->g_target != g_main->g_current);

    /* if the target saved stack is invalid (due to a failure to save it
    * during the exit of another tealet), we detect this here and
    * report an error
    * return value is:
    *  0 = successful switch
    *  1 = successful save only
    * -1 = error, couldn't save state
    * -2 = error, target tealet corrupt
    */
    if (g_main->g_target->stack == (tealet_stack_t*)-1)
        return TEALET_ERR_DEFUNCT;
    g_main->g_previous = g_main->g_current;
    {
        /* make sure that optimizers, e.g. gcc -O2, won't assume that
         * g_main->g_target stays unchanged across the switch and optimize it
         * into a register
         */
        tealet_sub_t * volatile *ptarget = &g_main->g_target;
        stackman_switch(tealet_save_restore_cb, (void*)g_main);
        g_main->g_target = *ptarget;
    }
    if (g_main->g_sw != SW_ERR) {
        g_main->g_current = g_main->g_target;
    } else {
        g_main->g_previous = previous;
        return TEALET_ERR_MEM;
    }
    g_main->g_target = NULL;
    return g_main->g_sw == SW_RESTORE ? 0 : 1;
}

/* We are initializing and switching to a new stub,
 * in order to immediately start a new tealet's execution.
 * stack_far is the far end of this stack and must be
 * far enough that local variables in this function get saved.
 * A stack variable in the calling function is sufficient.
 */
static int tealet_initialstub(tealet_main_t *g_main, tealet_run_t run, void *stack_far)
{
    int result;
    tealet_sub_t *g = g_main->g_current;
    tealet_sub_t *g_target = g_main->g_target;
    assert(g_target->stack == NULL); /* it is fresh */
    
    assert(run);

#if __GNUC__
    /* extra anti-inline protection */
    __asm__("");
#endif

    g_target->stack_far = (char *)stack_far;
    result = tealet_switchstack(g_main);
    if (result < 0) {
        /* couldn't allocate stack */
        g_main->g_current = g;
        return result;
    }
    if (result == 1) {
        /* We successfully saved the source state (our caller) and initialized
         * the current target without restoring state. We are the new tealet.
         */
        g = g_main->g_current;
        assert(g == g_target);
        assert(g->stack == NULL);     /* running */      
    
        #ifdef DEBUG_DUMP
        printf("starting %p\n", g);
        #endif
        g_target = (tealet_sub_t *)(run((tealet_t *)g, g_main->g_arg));
        #ifdef DEBUG_DUMP
        printf("ending %p -> %p\n", g, g_target);
        #endif
        if (tealet_exit((tealet_t*)g_target, NULL, TEALET_FLAG_DELETE))
            tealet_exit((tealet_t*)g_main, NULL, TEALET_FLAG_DELETE); /* failsafe */
        assert(!"This point should not be reached");
    } else {
        /* this is a switch back into the calling tealet */
        assert(result == 0);
    }
    return 0;
}


static tealet_sub_t *tealet_alloc_raw(tealet_main_t *g_main, tealet_alloc_t *alloc, size_t basesize, size_t extrasize)
{
    tealet_sub_t *g;
    size_t size = basesize + extrasize;
    g = (tealet_sub_t*) alloc->malloc_p(size, alloc->context);
    if (g == NULL)
        return NULL;
    if (g_main == NULL) {
        g_main = (tealet_main_t *)g;
        g_main->g_counter = 0;
    }
    g->base.main = (tealet_t *)g_main;
    if (extrasize)
        g->base.extra = (void*)((char*)g + basesize);
    else
        g->base.extra = NULL;
    g->stack = NULL;
    g->stack_far = NULL;
#ifndef NDEBUG
    g->id = 0;
#endif
#if TEALET_WITH_STATS
    g_main->g_counter++;
#ifndef NDEBUG
    g->id = g_main->g_counter;
#endif
#endif
    return g;
}

static tealet_sub_t *tealet_alloc_main(tealet_alloc_t *alloc, size_t extrasize)
{
    tealet_sub_t *result;
    size_t basesize = offsetof(tealet_main_t, _extra);
    if (alloc->context)
        alloc->context->main = NULL;
    result = tealet_alloc_raw(NULL, alloc, basesize, extrasize);
    if (alloc->context)
        alloc->context->main = &result->base;
    return result;
}

static tealet_sub_t *tealet_alloc(tealet_main_t *g_main)
{
    size_t basesize = offsetof(tealet_nonmain_t, _extra);
    size_t extrasize = g_main->g_extrasize;
    return tealet_alloc_raw(g_main, &g_main->g_alloc, basesize, extrasize);
}


/************************************************************/

tealet_t *tealet_initialize(tealet_alloc_t *alloc, size_t extrasize)
{
    tealet_sub_t *g;
    tealet_main_t *g_main;
    g = tealet_alloc_main(alloc, extrasize);
    if (g == NULL)
        return NULL;
    g_main = (tealet_main_t *)g;
    g->stack = NULL;
    g->stack_far = STACKMAN_SP_FURTHEST;
    g_main->g_user = NULL;
    g_main->g_current = g;
    g_main->g_previous = NULL;
    g_main->g_target = NULL;
    g_main->g_arg = NULL;
    g_main->g_alloc = *alloc;
    g_main->g_prev =  NULL;
    g_main->g_extrasize = extrasize;
#if TEALET_WITH_STATS
    /* init these.  the main tealet counts as one */
    g_main->g_tealets = 1;
    assert(g_main->g_counter == 1); /* set in alloc_raw */
#endif
    assert(TEALET_IS_MAIN_STACK(g_main));
    return (tealet_t *)g_main;
}

void tealet_finalize(tealet_t *tealet)
{
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    assert(TEALET_IS_MAIN_STACK(g_main));
    assert(g_main->g_current == (tealet_sub_t *)g_main);
    if(g_main->g_alloc.context)
        g_main->g_alloc.context->main = NULL;
    tealet_int_free(g_main, g_main);
}

void *tealet_malloc(tealet_t *tealet, size_t s)
{
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    return tealet_int_malloc(g_main, s);
}

void tealet_free(tealet_t *tealet, void *p)
{
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    tealet_int_free(g_main, p);
}

tealet_t *tealet_new(tealet_t *tealet, tealet_run_t run, void **parg)
{
    tealet_sub_t *result; /* store this until we return */
    int fail;
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    assert(TEALET_IS_MAIN_STACK(g_main));
    assert(!g_main->g_target);
    result = tealet_alloc(g_main);
    if (result == NULL)
        return NULL; /* Could not allocate */
    g_main->g_target = result;
    g_main->g_arg = parg ? *parg : NULL;
#if TEALET_WITH_STATS
    g_main->g_tealets ++;
#endif
    fail = tealet_initialstub(g_main, run, (void*)&result);
    if (fail) {
        /* could not save stack */
        tealet_int_free(g_main, result);
        g_main->g_target = NULL;
#if TEALET_WITH_STATS
        g_main->g_tealets --;
#endif
        return NULL;
    }
    if (parg)
        *parg = g_main->g_arg;
    return (tealet_t*)result;
}

int tealet_switch(tealet_t *stub, void **parg)
{
    tealet_sub_t *g_target = (tealet_sub_t *)stub;
    tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
    int result;
    if (g_target == g_main->g_current)
        return 0; /* switch to self */
#ifdef DEBUG_DUMP
    printf("switch %p -> %p\n", g_main->g_current, g_target);
#endif
    g_main->g_target = g_target;
    g_main->g_arg = parg ? *parg : NULL;
    result = tealet_switchstack(g_main);
    if (parg)
        *parg = g_main->g_arg;
#ifdef DEBUG_DUMP
    printf("done switch, res=%d, now in %p\n", result, g_main->g_current);
#endif
    return result;
}
 
int tealet_exit(tealet_t *target, void *arg, int flags)
{
    tealet_sub_t *g_target = (tealet_sub_t *)target;
    tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
    tealet_sub_t *g_current = g_main->g_current;
    char *stack_far = g_target->stack_far;
    int result;
    assert(g_current != (tealet_sub_t*)g_main); /* mustn't exit main */
    if (g_target == g_current)
        return -2; /* invalid tealet */

    g_current->stack_far = NULL; /* signal exit */
    assert (g_current->stack == NULL);
    if (!(flags & TEALET_FLAG_DELETE))
        g_current->stack = (tealet_stack_t*) -1; /* signal do-not-delete */
    g_main->g_target = g_target;
    g_main->g_arg = arg;
    result = tealet_switchstack(g_main);
    assert(result < 0); /* only return here if there was failure */
    g_target->stack_far = stack_far;
    g_current->stack = NULL;
    return result;
}

tealet_t *tealet_duplicate(tealet_t *tealet)
{
    tealet_sub_t *g_tealet = (tealet_sub_t *)tealet;
    tealet_main_t *g_main = TEALET_GET_MAIN(g_tealet);
    tealet_sub_t *g_copy;
    /* can't dup the current or the main tealet */
    assert(g_tealet != g_main->g_current && g_tealet != (tealet_sub_t*)g_main);
    g_copy = tealet_alloc(g_main);
    if (g_copy == NULL)
        return NULL;
#if TEALET_WITH_STATS
    g_main->g_tealets++;
#endif
    g_copy->stack_far = g_tealet->stack_far;
    g_copy->stack = tealet_stack_dup(g_tealet->stack);
    if (g_main->g_extrasize)
        memcpy(g_copy->base.extra, g_tealet->base.extra, g_main->g_extrasize);
    return (tealet_t*)g_copy;
}

void tealet_delete(tealet_t *target)
{
    tealet_sub_t *g_target = (tealet_sub_t *)target;
    tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
    assert(!TEALET_IS_MAIN(target));
    tealet_stack_decref(g_main, g_target->stack);
    tealet_int_free(g_main, g_target);
#if TEALET_WITH_STATS
    g_main->g_tealets--;
#endif
}

tealet_t *tealet_current(tealet_t *tealet)
{
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    return (tealet_t *)g_main->g_current;
}

tealet_t *tealet_previous(tealet_t *tealet)
{
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    return (tealet_t *)g_main->g_previous;
}

void **tealet_main_userpointer(tealet_t *tealet)
{
   tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
   return &g_main->g_user;
}

int tealet_status(tealet_t *_tealet)
{
    tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
    if (tealet->stack_far == NULL)
        return TEALET_STATUS_EXITED;
    if (tealet->stack == (tealet_stack_t*)-1)
        return TEALET_STATUS_DEFUNCT;
    return TEALET_STATUS_ACTIVE;
}

void tealet_get_stats(tealet_t *tealet, tealet_stats_t *stats)
{
#if ! TEALET_WITH_STATS
    memset(stats, 0, sizeof(*stats));
#else
    tealet_main_t *tmain = TEALET_GET_MAIN(tealet);
    stats->n_active = tmain->g_tealets;
    stats->n_total = tmain->g_counter;
#endif
}

ptrdiff_t tealet_stack_diff(void *a, void *b)
{
    return STACKMAN_SP_DIFF((ptrdiff_t)a, (ptrdiff_t)(b));
}

void *tealet_get_far(tealet_t *_tealet)
{
    tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
    return tealet->stack_far;
}

#if __GNUC__ > 4
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-local-addr"
#elif _MSC_VER
#pragma warning(push)
#pragma warning( disable : 4172 )
#endif
void *tealet_new_far(tealet_t *d1, tealet_run_t d2, void **d3)
{
    tealet_sub_t *result;
    void *r;
    (void)d1;
    (void)d2;
    (void)d3;
    /* avoid compiler warnings about returning tmp addr */
    r = (void*)&result;
    return r;
}
#if __GNUC__ > 4
#pragma GCC diagnostic pop
#elif _MSC_VER
#pragma warning(pop)
#endif

size_t tealet_get_stacksize(tealet_t *_tealet)
{
    tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
    if (tealet->stack)
        return tealet_stack_getsize(tealet->stack);
    return 0;
}
