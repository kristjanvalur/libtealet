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


/* enable collection of tealet stats - default enabled, define TEALET_WITH_STATS=0 to disable */
#ifndef TEALET_WITH_STATS
#define TEALET_WITH_STATS 1
#endif


/************************************************************/

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
    char *stack_far;                /* the far boundary of this stack (or STACKMAN_SP_FURTHEST for unbounded) */
    size_t saved;                   /* total amount of memory saved in all chunks */
    struct tealet_chunk_t chunk;    /* the initial chunk */
} tealet_stack_t;


/* the actual tealet structure as used internally 
 * The main tealet will have stack_far set to STACKMAN_SP_FURTHEST,
 * representing an unbounded stack extent (the entire process stack).
 * "stack" is zero for a running tealet, otherwise it points
 * to the saved stack, or is -1 if the state is invalid.
 * In addition, stack_far is set to NULL value to indicate
 * that a tealet is exiting.
 */
typedef struct tealet_sub_t {
  tealet_t base;				   /* the public part of the tealet */
  char *stack_far;                 /* the "far" end of the stack, NULL when exiting, or STACKMAN_SP_FURTHEST for unbounded */
  tealet_stack_t *stack;           /* saved stack or 0 if active or -1 if invalid*/
#if TEALET_WITH_STATS
  struct tealet_sub_t *next_tealet; /* next in circular list of all tealets */
  struct tealet_sub_t *prev_tealet; /* prev in circular list of all tealets */
#endif
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
  int           g_flags;     /* default flags when tealet exits */
  int g_tealets;            /* number of active tealets excluding main */
  int g_counter;            /* total number of tealets */
#if TEALET_WITH_STATS
  /* Extended memory statistics */
  size_t g_bytes_allocated;       /* Current heap allocation */
  size_t g_bytes_allocated_peak;  /* Peak heap allocation */
  size_t g_blocks_allocated;      /* Current number of allocated blocks */
  size_t g_blocks_allocated_peak; /* Peak number of allocated blocks */
  size_t g_blocks_allocated_total;/* Total allocation calls */
  size_t g_stack_bytes;           /* Bytes used for stack storage */
  size_t g_stack_count;           /* Number of stack structures currently allocated */
  size_t g_stack_chunk_count;     /* Number of stack chunks currently allocated (including initial) */
#endif
  size_t       g_extrasize; /* amount of extra memory in tealets */
  double _extra[1];         /* start of any extra data */
} tealet_main_t;

#define TEALET_IS_MAIN_STACK(t)  (((tealet_sub_t *)(t))->stack_far == STACKMAN_SP_FURTHEST)
#define TEALET_GET_MAIN(t)     ((tealet_main_t *)(((tealet_t *)(t))->main))

/************************************************************
 * Statistics tracking macros
 */
#if TEALET_WITH_STATS
/* Forward declaration for list verification */
/* Link a tealet into the circular list after main tealet */
#define TEALET_LIST_ADD(main, t) do { \
    (t)->next_tealet = (main)->base.next_tealet; \
    (t)->prev_tealet = (tealet_sub_t*)(main); \
    (main)->base.next_tealet->prev_tealet = (t); \
    (main)->base.next_tealet = (t); \
} while (0)

/* Unlink a tealet from the circular list */
#define TEALET_LIST_REMOVE(t) do { \
    (t)->prev_tealet->next_tealet = (t)->next_tealet; \
    (t)->next_tealet->prev_tealet = (t)->prev_tealet; \
} while (0)

#define STATS_ADD_ALLOC(main, size) do { \
    (main)->g_bytes_allocated += (size); \
    (main)->g_blocks_allocated++; \
    (main)->g_blocks_allocated_total++; \
    if ((main)->g_bytes_allocated > (main)->g_bytes_allocated_peak) \
        (main)->g_bytes_allocated_peak = (main)->g_bytes_allocated; \
    if ((main)->g_blocks_allocated > (main)->g_blocks_allocated_peak) \
        (main)->g_blocks_allocated_peak = (main)->g_blocks_allocated; \
} while(0)

#define STATS_SUB_ALLOC(main, size) do { \
    (main)->g_bytes_allocated -= (size); \
    (main)->g_blocks_allocated--; \
} while(0)
#else
#define TEALET_LIST_ADD(main, t) ((void)0)
#define TEALET_LIST_REMOVE(t) ((void)0)
#define STATS_ADD_ALLOC(main, size) ((void)0)
#define STATS_SUB_ALLOC(main, size) ((void)0)
#endif

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

/* Free a tealet, unlinking it from the circular list first */
static void tealet_free_tealet(tealet_main_t *main, tealet_sub_t *t)
{
    size_t basesize = offsetof(tealet_nonmain_t, _extra);
    size_t size = basesize + main->g_extrasize;
#if TEALET_WITH_STATS
    TEALET_LIST_REMOVE(t);
#endif
    STATS_SUB_ALLOC(main, size);
    tealet_int_free(main, t);
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
    STATS_ADD_ALLOC(main, tsize);
#if TEALET_WITH_STATS
    main->g_stack_count++;
    main->g_stack_chunk_count++;  /* Initial chunk counts */
    main->g_stack_bytes += tsize;
#endif
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
    STATS_ADD_ALLOC(main, tsize);
#if TEALET_WITH_STATS
    main->g_stack_chunk_count++;  /* Additional chunk */
    main->g_stack_bytes += tsize;
#endif
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
    STATS_SUB_ALLOC(main, offsetof(tealet_stack_t, chunk.data[0]) + stack->chunk.size);
#if TEALET_WITH_STATS
    main->g_stack_count--;
    main->g_stack_chunk_count--;  /* Initial chunk */
    main->g_stack_bytes -= offsetof(tealet_stack_t, chunk.data[0]) + stack->chunk.size;
#endif
    tealet_int_free(main, (void*)stack);
    while(chunk) {
        tealet_chunk_t *next = chunk->next;
        STATS_SUB_ALLOC(main, offsetof(tealet_chunk_t, data[0]) + chunk->size);
#if TEALET_WITH_STATS
        main->g_stack_chunk_count--;  /* Additional chunk */
        main->g_stack_bytes -= offsetof(tealet_chunk_t, data[0]) + chunk->size;
#endif
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
        STATS_SUB_ALLOC(main, offsetof(tealet_chunk_t, data[0]) + chunk->size);
#if TEALET_WITH_STATS
        main->g_stack_chunk_count--;  /* Additional chunk being made defunct */
        main->g_stack_bytes -= offsetof(tealet_chunk_t, data[0]) + chunk->size;
#endif
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
            tealet_free_tealet(g_main, g_current);
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
    
    /* stackman switch is an external function so an optizer
     * cannot assume that any pointers reachable by
     * g_main stay unchanged across the switch
     */
    stackman_switch(tealet_save_restore_cb, (void*)g_main);
    
    if (g_main->g_sw != SW_ERR) {
        g_main->g_current = g_main->g_target;
    } else {
        g_main->g_previous = previous;
        return TEALET_ERR_MEM;
    }
    g_main->g_target = NULL;
    return g_main->g_sw == SW_RESTORE ? 0 : 1;
}

/* We are initializing a new tealet, either switching to it and
 * running it, or switching from it (saving its virgin stack) back
 * to the caller, in order to switch to it later and run it.
 * stack_far is the far end of this stack and must be
 * far enough that local variables in this function get saved.
 * A stack variable in the calling function is sufficient.
 */
static int tealet_initialstub(tealet_main_t *g_main, tealet_sub_t *g_new, tealet_run_t run, void *stack_far, int run_on_create)
{
    int result;
    tealet_sub_t *g_target;
    assert(g_new->stack == NULL); /* it is fresh */    
    assert(run);

    g_new->stack_far = (char *)stack_far;
    result = tealet_switchstack(g_main);
    if (result < 0) {
        /* couldn't allocate stack */
        return result;
    }
 
    assert(result == 0 || result == 1);
    /* 'result' is 1 if this was just the neccary stack 'save' to create
     * a new tealetlet, with no restore of an existing stack
     */
    if (run_on_create == result) {
        /* need to run the actual code.  In the 'run_on_create' case this is
         * done on the initial save.  The current teallet is the new teallet,
         * the previous tealet's stack was saved, and we run as then new one.
         * In the '!run_on_create' case, the initial save was the new teallet
         * and we just returned immediately to the calling one.  We are now
         * returning here on a switch, to run the teallet
         */
        
        /* the following assertion may be invalid, if a tealet_create() tealet
         * was duplicated.  We may now be a copy
         */
        if (run_on_create)
            assert(g_main->g_current == g_new);        /* only valid for tealet_new */
        assert(g_main->g_current->stack == NULL);     /* running */      
    
        g_target = (tealet_sub_t *)(run((tealet_t *)g_main->g_current, g_main->g_arg));
        tealet_exit((tealet_t*)g_target, NULL, TEALET_FLAG_DELETE);
        assert(!"This point should not be reached");
    } else {
        /* Either just a create, with no run, or a switch back
         * into the tealet_new()
         */
        ;
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
#if TEALET_WITH_STATS
        g_main->g_counter = 0;
        /* Initialize stats before tracking this allocation */
        g_main->g_bytes_allocated = 0;
        g_main->g_blocks_allocated = 0;
        g_main->g_blocks_allocated_total = 0;
#endif
    }
    /* Track tealet structure allocation */
    STATS_ADD_ALLOC(g_main, size);
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
    /* Link into the circular list (but not the main tealet itself during init) */
    if (g != (tealet_sub_t*)g_main) {
        TEALET_LIST_ADD(g_main, g);
    }
#endif
    return g;
}

static tealet_sub_t *tealet_alloc_main(tealet_alloc_t *alloc, size_t extrasize)
{
    size_t basesize = offsetof(tealet_main_t, _extra);
    return tealet_alloc_raw(NULL, alloc, basesize, extrasize);
}

static tealet_sub_t *tealet_alloc(tealet_main_t *g_main)
{
    tealet_sub_t *result;
    size_t basesize = offsetof(tealet_nonmain_t, _extra);
    size_t extrasize = g_main->g_extrasize;
    result = tealet_alloc_raw(g_main, &g_main->g_alloc, basesize, extrasize);
#if TEALET_WITH_STATS
    g_main->g_tealets++;
#endif
    return result;
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
    g_main->g_sw = SW_NOP;
    g_main->g_flags = 0;
#if TEALET_WITH_STATS
    /* Initialize circular list - main tealet points to itself */
    g->next_tealet = g;
    g->prev_tealet = g;
    /* init these.  the main tealet counts as one */
    g_main->g_tealets = 1;
    assert(g_main->g_counter == 1); /* set in alloc_raw */
    /* bytes_allocated, blocks_allocated already set in tealet_alloc_raw */
    g_main->g_bytes_allocated_peak = g_main->g_bytes_allocated;
    g_main->g_blocks_allocated_peak = g_main->g_blocks_allocated;
    g_main->g_stack_bytes = 0;
    g_main->g_stack_count = 0;
    g_main->g_stack_chunk_count = 0;
#endif
    assert(TEALET_IS_MAIN_STACK(g_main));
    return (tealet_t *)g_main;
}

void tealet_finalize(tealet_t *tealet)
{
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    assert(TEALET_IS_MAIN_STACK(g_main));
    assert(g_main->g_current == (tealet_sub_t *)g_main);
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

/* create a tealet by saving the current stack and starting 
 * immediate execution of a new one
 */
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
    fail = tealet_initialstub(g_main, result, run, (void*)&result, 1);
    if (fail) {
        /* could not save stack */
        tealet_delete((tealet_t*)result);
        g_main->g_target = NULL;
        return NULL;
    }
    if (parg)
        *parg = g_main->g_arg;
    return (tealet_t*)result;
}

/* create a tealet by saving the target stack and switching
 * back to the caller, allowing the caller to run the
 * tealet proper later, by switching to it.
 */
tealet_t *tealet_create(tealet_t *tealet, tealet_run_t run)
{
    tealet_sub_t *result; /* store this until we return */
    int fail;
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    tealet_sub_t *previous = g_main->g_previous;
    assert(TEALET_IS_MAIN_STACK(g_main));
    assert(!g_main->g_target);
    result = tealet_alloc(g_main);
    if (result == NULL)
        return NULL; /* Could not allocate */
    /* we turn into the new tealet and switch back, in order
     * to save the new tealet's stack at this position
     */
    g_main->g_target = g_main->g_current;
    g_main->g_current = result;
    fail = tealet_initialstub(g_main, result, run, (void*)&result, 0);
    if (fail) {
        /* could not save stack */
        tealet_delete((tealet_t*)result);
        g_main->g_current = g_main->g_target;
        g_main->g_target = NULL;
        return NULL;
    } else {
        /* restore g_previous to whatever it was.  We don't count
         * this switch from the temporary tealet back to us
         * as proper switch in that sense
         */
        g_main->g_previous = previous;
    }
    return (tealet_t*)result;
}

int tealet_switch(tealet_t *stub, void **parg)
{
    tealet_sub_t *g_target = (tealet_sub_t *)stub;
    tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
    int result;
    if (g_target == g_main->g_current) {
        g_main->g_previous = g_main->g_current;
        return 0; /* switch to self */
    }
    g_main->g_target = g_target;
    g_main->g_arg = parg ? *parg : NULL;
    result = tealet_switchstack(g_main);
    if (parg)
        *parg = g_main->g_arg;
    return result;
}
 
static int tealet_exit_inner(tealet_t *target, void *arg, int flags)
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

int tealet_exit(tealet_t *target, void *arg, int flags)
{
    tealet_sub_t *g_target = (tealet_sub_t *)target;
    tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
    int result;
    if (flags & TEALET_FLAG_DEFER)
    {
        /* setting up arg and flags for the run() return value */
        g_main->g_arg = arg;
        g_main->g_flags = flags;
        return 0;
    }
    if (g_main->g_flags & TEALET_FLAG_DEFER)
    {
        /* Called second time (e.g. from return of run())
         * use arg and flags from last time
         */
        flags = g_main->g_flags & (~TEALET_FLAG_DEFER);
        arg = g_main->g_arg;
        g_main->g_flags = 0;
        g_main->g_arg = 0;
    }
    result = tealet_exit_inner(target, arg, flags);
    assert(result < 0);
    /* fallback: switch to main */
    result = tealet_exit_inner(target->main, arg, flags);
    /* should never reach here */
    assert(0);
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
    g_copy->stack_far = g_tealet->stack_far;
    /* can't fail */
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
    tealet_free_tealet(g_main, g_target);
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
    (void)tealet; /* unused */
    memset(stats, 0, sizeof(*stats));
#else
    tealet_main_t *tmain = TEALET_GET_MAIN(tealet);
    
    /* Basic tealet counts */
    stats->n_active = tmain->g_tealets;
    stats->n_total = tmain->g_counter;
    
    /* Memory usage statistics */
    stats->bytes_allocated = tmain->g_bytes_allocated;
    stats->bytes_allocated_peak = tmain->g_bytes_allocated_peak;
    stats->blocks_allocated = tmain->g_blocks_allocated;
    stats->blocks_allocated_peak = tmain->g_blocks_allocated_peak;
    stats->blocks_allocated_total = tmain->g_blocks_allocated_total;
    
    /* Stack memory storage statistics - from tracked values */
    stats->stack_bytes = tmain->g_stack_bytes;
    stats->stack_count = tmain->g_stack_count;
    stats->stack_chunk_count = tmain->g_stack_chunk_count;
    
    /* Compute expanded and naive sizes by walking all tealets */
    stats->stack_bytes_expanded = 0;
    stats->stack_bytes_naive = 0;
    
    /* Walk the circular list of all tealets */
    tealet_sub_t *start = (tealet_sub_t*)tmain;
    tealet_sub_t *t = start;
    int stack_num = 0;
    int tealet_count = 0;
    do {
        tealet_count++;
        /* Count tealets with saved stacks (current tealet won't have stack saved) */
        if (t->stack && t->stack != (tealet_stack_t*)-1) {
            tealet_stack_t *stack = t->stack;
            tealet_chunk_t *chunk;
            size_t this_naive = 0;
            size_t this_expanded = 0;
            void *effective_far;
            
            /* Compute the effective "far" boundary for naive calculation */
            if (stack->stack_far == STACKMAN_SP_FURTHEST) {
                /* For unbounded stacks, recompute the furthest point from actual chunks */
                /* Compute far = near + size for the initial chunk */
                effective_far = (void*)STACKMAN_SP_ADD((ptrdiff_t)stack->chunk.stack_near, 
                                                       (ptrdiff_t)stack->chunk.size);
                chunk = stack->chunk.next;
                while (chunk) {
                    /* Compute far for this chunk and keep the furthest */
                    void *chunk_far = (void*)STACKMAN_SP_ADD((ptrdiff_t)chunk->stack_near,
                                                             (ptrdiff_t)chunk->size);
                    if (STACKMAN_SP_DIFF((ptrdiff_t)chunk_far, (ptrdiff_t)effective_far) > 0)
                        effective_far = chunk_far;
                    chunk = chunk->next;
                }
            } else {
                /* For bounded stacks, use the recorded far boundary */
                effective_far = stack->stack_far;
            }
            
            /* Compute naive size: extent from effective_far to near, plus overhead */
            size_t extent = (size_t)STACKMAN_SP_DIFF((ptrdiff_t)effective_far,
                                                      (ptrdiff_t)stack->chunk.stack_near);
            this_naive = offsetof(tealet_stack_t, chunk.data[0]) + extent;
            stats->stack_bytes_naive += this_naive;
            
            /* Add up all chunk allocations for this tealet (counts shared chunks multiple times) */
            /* Initial chunk is part of stack structure */
            this_expanded = offsetof(tealet_stack_t, chunk.data[0]) + stack->chunk.size;
            
            /* Count additional chunks */
            chunk = stack->chunk.next;
            int chunk_count = 1;
            while (chunk) {
                this_expanded += offsetof(tealet_chunk_t, data[0]) + chunk->size;
                chunk_count++;
                chunk = chunk->next;
            }
            stats->stack_bytes_expanded += this_expanded;
            
            stack_num++;
        }
        t = t->next_tealet;
    } while (t != start);
#endif
}

void tealet_reset_peak_stats(tealet_t *tealet)
{
#if TEALET_WITH_STATS
    tealet_main_t *tmain = TEALET_GET_MAIN(tealet);
    tmain->g_bytes_allocated_peak = tmain->g_bytes_allocated;
    tmain->g_blocks_allocated_peak = tmain->g_blocks_allocated;
    /* Note: We don't track stack_bytes_naive_peak as it would require walking
     * all tealets on every allocation. stack_bytes_naive is computed on demand. */
#else
    (void)tealet; /* unused */
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

int tealet_set_far(tealet_t *_tealet, void *far_boundary)
{
    tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
    tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
    
    /* Only the main tealet can have its far boundary set */
    if (!TEALET_IS_MAIN(_tealet))
        return -1;
    
    /* Verify we're being called from the main tealet (it must be current) */
    if (g_main->g_current != tealet)
        return -1;
    
    /* Set the far boundary */
    tealet->stack_far = (char *)far_boundary;
    return 0;
}

int tealet_fork(tealet_t *_current, int flags)
{
    tealet_sub_t *g_current = (tealet_sub_t *)_current;
    tealet_main_t *g_main = TEALET_GET_MAIN(g_current);
    tealet_sub_t *g_child;
    int child_id;
    
    /* Can't fork the main tealet */
    if (TEALET_IS_MAIN(_current))
        return TEALET_ERR_DEFUNCT;
    
    /* Main tealet must have a bounded stack (far boundary set) */
    if (TEALET_IS_MAIN_STACK((tealet_sub_t*)g_main))
        return TEALET_ERR_DEFUNCT;
    
    /* Current tealet must be active (we are currently executing it) */
    if (g_main->g_current != g_current)
        return TEALET_ERR_DEFUNCT;
    
    /* Current tealet must not be suspended (stack must be NULL when active) */
    if (g_current->stack != NULL)
        return TEALET_ERR_DEFUNCT;
    
    /* Allocate the child tealet */
    g_child = tealet_alloc(g_main);
    if (g_child == NULL)
        return TEALET_ERR_MEM;
    
    /* Copy the far boundary */
    g_child->stack_far = g_current->stack_far;
    
    /* Save the current tealet's stack by doing a fake switch to main.
     * We use the same mechanism as tealet_switch, but we'll abort before
     * actually switching to avoid changing the current tealet.
     * 
     * The trick: we'll temporarily set g_target to main, trigger the save,
     * then duplicate the saved stack for the child.
     */
    {
        void *old_sp;
        tealet_sub_t *saved_target = g_main->g_target;
        tealet_sub_t *saved_previous = g_main->g_previous;
        tealet_stack_t *saved_stack;
        
        /* Prepare for a "fake" switch to main to trigger stack save */
        g_main->g_target = (tealet_sub_t *)g_main;
        g_main->g_previous = g_current;
        
        /* Get the current stack pointer (approximately) */
        old_sp = &old_sp;
        
        /* Save the current stack */
        if (tealet_save_state(g_main, old_sp) < 0) {
            /* Failed to save stack */
            g_main->g_target = saved_target;
            g_main->g_previous = saved_previous;
            tealet_free_tealet(g_main, g_child);
            return TEALET_ERR_MEM;
        }
        
        /* Now g_current->stack contains the saved stack */
        saved_stack = g_current->stack;
        assert(saved_stack != NULL);
        
        /* Duplicate the stack for the child */
        g_child->stack = tealet_stack_dup(saved_stack);
        
        /* Restore the saved stack pointer back to current (we're not really switching) */
        g_current->stack = saved_stack;
        
        /* Restore g_main state */
        g_main->g_target = saved_target;
        g_main->g_previous = saved_previous;
    }
    
    /* Copy extra data if present */
    if (g_main->g_extrasize)
        memcpy(g_child->base.extra, g_current->base.extra, g_main->g_extrasize);
    
    /* Get child ID for return value (use the debug id if available, otherwise use pointer) */
#ifndef NDEBUG
    child_id = g_child->id;
#else
    child_id = (int)(ptrdiff_t)g_child; /* Use pointer as ID, ensure it's positive */
    if (child_id <= 0)
        child_id = 1; /* Ensure positive */
#endif
    
    /* If TEALET_FORK_SWITCH flag is set, switch to the child immediately */
    if (flags & TEALET_FORK_SWITCH) {
        void *dummy_arg = NULL;
        int switch_result = tealet_switch((tealet_t *)g_child, &dummy_arg);
        if (switch_result < 0) {
            /* Switch failed - clean up and return error */
            tealet_delete((tealet_t *)g_child);
            return switch_result;
        }
        /* We've returned from the child - we are now in the parent */
        /* The child will execute below and return 0 */
    }
    
    /* At this point, we need to determine if we're the parent or child.
     * After a switch (if TEALET_FORK_SWITCH was set), g_current would have
     * changed. If current tealet is g_child, we're the child. Otherwise parent.
     */
    if (g_main->g_current == g_child) {
        /* We are the child - return 0 */
        return 0;
    } else {
        /* We are the parent - return child ID */
        return child_id;
    }
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
