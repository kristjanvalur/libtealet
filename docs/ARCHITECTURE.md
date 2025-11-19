# libtealet Architecture

This document explains the internal design of libtealet, focusing on the stack-slicing technique and memory optimization strategies.

## Stack Direction Terminology

⚠️ **Important Concept:** libtealet uses platform-agnostic "near" and "far" terminology for stack boundaries.

### The Problem

C stacks grow in different directions on different platforms:
- **Descending stacks** (most common: x86, ARM): Stack pointer decreases as functions are called
  - Stack "bottom" = high memory address
  - Stack "top" = low memory address (where SP currently points)
- **Ascending stacks** (rare): Stack pointer increases
  - Stack "bottom" = low memory address  
  - Stack "top" = high memory address

The traditional "top/bottom" metaphor is ambiguous:
- In the stack-of-cards metaphor: "bottom" = oldest (first), "top" = newest (last)
- In memory addresses: depends on whether stack grows up or down

### The Solution: Near and Far

libtealet uses direction-neutral terminology:

- **Near boundary**: The current stack position (where the stack pointer is now)
  - Closest to the active function's frame
  - Changes as the stack grows/shrinks
  - In a descending stack: lower address
  - In an ascending stack: higher address

- **Far boundary**: The stack limit (where the stack started or cannot grow beyond)
  - Furthest from the current position
  - Fixed reference point for each tealet
  - In a descending stack: higher address (original entry point)
  - In an ascending stack: lower address

**Mnemonic:** Think of a telescope extending:
- Near = the eyepiece you're looking through (current position)
- Far = the distant objective lens (fixed reference point)

**Example on x86-64 (descending stack):**
```
High addresses →  0x7fff8000  ← Far boundary (main entry, stack "bottom")
                  0x7fff7f00  ← Function A's frame
                  0x7fff7e00  ← Function B's frame  
Low addresses  →  0x7fff7d00  ← Near boundary (current SP, stack "top")
                     ↑
                  Stack grows downward
```

**Same logical stack on ascending platform:**
```
Low addresses  →  0x1000      ← Far boundary (main entry)
                  0x1100      ← Function A's frame
                  0x1200      ← Function B's frame
High addresses →  0x1300      ← Near boundary (current SP)
                     ↑
                  Stack grows upward
```

In both cases:
- **Near** = current execution point
- **Far** = original entry point
- Saved stack = memory between near and far

This abstraction allows the same algorithm to work regardless of platform stack direction.

## Overview

libtealet implements **symmetric coroutines** through stack-slicing: saving portions of the C execution stack to the heap and restoring them later. The innovation lies in **incremental stack saving** that minimizes memory usage.

## Core Data Structures

### tealet_chunk_t - Stack Segment

```c
typedef struct tealet_chunk_t {
    struct tealet_chunk_t *next;   /* Link to next chunk */
    char *stack_near;              /* Near boundary (saved position) */
    size_t size;                   /* Bytes in this chunk */
    char data[1];                  /* Stack data follows */
} tealet_chunk_t;
```

A chunk represents a contiguous saved stack segment. Multiple chunks can be chained together.

### tealet_stack_t - Complete Saved Stack

```c
typedef struct tealet_stack_t {
    int refcount;                  /* Reference count for sharing */
    struct tealet_stack_t **prev;  /* Doubly-linked list pointers */
    struct tealet_stack_t *next;
    char *stack_far;               /* Far boundary of stack */
    size_t saved;                  /* Total bytes across all chunks */
    struct tealet_chunk_t chunk;   /* First chunk (inline) */
} tealet_stack_t;
```

**Reference counting** allows stack sharing when duplicating tealets.

**Doubly-linked list** (`prev`/`next`) tracks partially-saved stacks in the `g_prev` chain.

### tealet_sub_t - Tealet Instance

```c
typedef struct tealet_sub_t {
    tealet_t base;                 /* Public API surface */
    char *stack_far;               /* Stack limit (NULL = exiting) */
    tealet_stack_t *stack;         /* NULL=active, -1=defunct, ptr=saved */
    int id;                        /* Debug identifier */
} tealet_sub_t;
```

**Stack states:**
- `NULL`: Tealet is currently running (on the C stack)
- `(tealet_stack_t*)-1`: Defunct (corrupted, unusable)
- `tealet_stack_t*`: Saved stack data

**stack_far** indicates:
- Valid pointer: Normal tealet (bounded stack extent)
- `NULL`: Tealet is exiting
- `STACKMAN_SP_FURTHEST`: Unbounded stack (main tealet)

The main tealet uses `STACKMAN_SP_FURTHEST` because it runs on the process's original C stack, which could theoretically extend all the way to the start of the process. This special value means "save as much as needed" without a predefined limit. When computing stack statistics, tealets with `STACKMAN_SP_FURTHEST` have their naive size calculated from actual saved chunks rather than the full theoretical extent (which would be the entire address space).

### tealet_main_t - Master Tealet

```c
typedef struct tealet_main_t {
    tealet_sub_t base;
    void *g_user;                  /* User data pointer */
    tealet_sub_t *g_current;       /* Currently executing tealet */
    tealet_sub_t *g_previous;      /* Who switched to us */
    tealet_sub_t *g_target;        /* Switch target (temporary) */
    void *g_arg;                   /* Argument passing channel */
    tealet_alloc_t g_alloc;        /* Memory allocator */
    tealet_stack_t *g_prev;        /* ⭐ PARTIALLY-SAVED STACKS CHAIN */
    tealet_sr_e g_sw;              /* Save/restore state machine */
    int g_flags;                   /* Exit flags */
    int g_tealets, g_counter;      /* Statistics */
    size_t g_extrasize;            /* Extra data per tealet */
} tealet_main_t;
```

The `g_prev` field is the linchpin of the memory optimization strategy.

## Stack-Chaining Optimization

### The Problem

When tealet A switches to tealet B, A's stack must be saved. But how much?

**Naive approach:** Save A's entire stack
- Wastes memory (most of stack may be unused)
- Slow (unnecessary copying)

**Challenge:** A might later switch to tealet C that's even deeper on the stack.

### The Solution: Incremental Saving with Chaining

libtealet saves stacks **incrementally** based on need:

1. When A switches to B, save A's stack only **up to B's far boundary**
2. Link A's partially-saved stack into the `g_prev` chain
3. If A later switches to C (deeper than B), **grow** A's saved stack

**Memory savings:** Only the differential portions are saved, not complete stack copies.

### Visual Example

```
High Memory (stack bottom)
    ↑
    |  STACKMAN_SP_FURTHEST (main's far boundary)
    |  ┌─────────────────┐
    |  │   Main stack    │
    |  └─────────────────┘
    ├─ A's stack_far
    |  ┌─────────────────┐
    |  │   A's stack     │ ← Created tealet A here
    |  └─────────────────┘
    ├─ B's stack_far  
    |  ┌─────────────────┐
    |  │   B's stack     │ ← Created tealet B deeper
    |  └─────────────────┘
    ├─ C's stack_far
    |  ┌─────────────────┐
    |  │   C's stack     │ ← Created tealet C even deeper
    |  └─────────────────┘
    ↓
Low Memory (stack top)
```

### Switch Sequence Example

**Step 1: Main → A**
```
Action: Main switches to A
Saved: Nothing (A is new, becomes active)
g_prev: Empty
```

**Step 2: A → B**
```
Action: A switches to B
Saved: A's stack from current position up to B's stack_far
g_prev: [A] (partially saved)

A's tealet_stack_t:
  chunk.size = distance(A's position, B's stack_far)
  saved = chunk.size
```

**Step 3: B → A**
```
Action: B switches back to A
Saved: B's stack up to A's stack_far
g_prev: [B, A] (both partially saved)
Result: A's stack restored, A continues
```

**Step 4: A → C**
```
Action: A switches to C (C is deeper than B)
Saved: A's stack GROWS from B's limit to C's limit
g_prev: [A] (grown), B removed if fully saved

A's tealet_stack_t grows:
  new_chunk = malloc(...)
  new_chunk.size = distance(B's stack_far, C's stack_far)
  new_chunk.next = chunk.next
  chunk.next = new_chunk
  saved += new_chunk.size
```

### The g_prev Chain

The `g_main->g_prev` chain tracks **partially-saved stacks**:

```c
g_main->g_prev → stackA → stackB → stackC → NULL
                   ↑         ↑         ↑
                 (A)       (B)       (C)
               partial   partial   partial
```

**Walk the chain** during switch to grow stacks as needed:

```c
static int tealet_stack_grow_list(tealet_main_t *main, 
    tealet_stack_t *list, char *saveto, tealet_stack_t *target, 
    int fail_ok)
{
    while (list) {
        if (list == target) {
            /* Reached target, stop here */
            tealet_stack_unlink(list);
            return 0;
        }
        
        /* Grow this stack up to saveto */
        int full;
        int fail = tealet_stack_growto(main, list, saveto, &full, fail_ok);
        if (fail) return fail;
        
        /* If fully saved, remove from chain */
        if (full)
            tealet_stack_unlink(list);
        
        list = list->next;
    }
    return 0;
}
```

**Unlink when fully saved:** Once a stack is saved entirely (up to its `stack_far`), remove it from the chain.

## Stack Growth Algorithm

```c
static int tealet_stack_grow(tealet_main_t *main,
    tealet_stack_t *stack, size_t size)
{
    size_t diff = size - stack->saved;  /* How much more to save */
    
    /* Allocate new chunk for differential */
    tealet_chunk_t *chunk = malloc(sizeof(tealet_chunk_t) + diff);
    
    /* Copy additional stack data */
    memcpy(&chunk->data[0], stack_near + stack->saved, diff);
    
    /* Link into chunk chain */
    chunk->next = stack->chunk.next;
    stack->chunk.next = chunk;
    stack->saved = size;
    
    return 0;
}
```

Stacks are **chains of chunks**, grown on demand.

## Stack Restore

```c
static void tealet_stack_restore(tealet_stack_t *stack)
{
    tealet_chunk_t *chunk = &stack->chunk;
    
    /* Walk chunk chain, restoring each segment */
    do {
        memcpy(chunk->stack_near, &chunk->data[0], chunk->size);
        chunk = chunk->next;
    } while (chunk);
}
```

Simple iteration through chunks, copying each back to the C stack.

## State Transitions

A tealet progresses through these states:

```
    [CREATED]
       ↓
    tealet_create() / tealet_new()
       ↓
    [ACTIVE] ←──────┐
       ↓            │
    tealet_switch() │ (restore)
       ↓            │
    [SAVED] ────────┘
       ↓
    run() returns
       ↓
    [EXITED] or [DEFUNCT]
```

### State Indicators

**stack pointer:**
- `NULL`: Active (currently running)
- `tealet_stack_t*`: Saved (suspended)
- `(tealet_stack_t*)-1`: Defunct (corrupt)

**stack_far pointer:**
- Valid address: Normal bounded stack
- `NULL`: Exiting (run function returning)
- `STACKMAN_SP_FURTHEST`: Unbounded stack (typically main tealet)

The special value `STACKMAN_SP_FURTHEST` represents an unbounded stack extent, used by the main tealet since it runs on the original process stack with no predetermined limit.

### Defunct State

A tealet becomes defunct if:
1. Stack couldn't be saved due to memory shortage during exit
2. Explicitly marked after corruption

**Recovery:** None. Defunct tealets cannot be used and should be deleted.

```c
if (tealet_status(t) == TEALET_STATUS_DEFUNCT) {
    tealet_delete(t);  /* Clean up */
}
```

## Reference Counting

Stacks use reference counting for sharing:

```c
static tealet_stack_t *tealet_stack_dup(tealet_stack_t *stack) {
    stack->refcount += 1;
    return stack;
}

static void tealet_stack_decref(tealet_main_t *main, 
    tealet_stack_t *stack)
{
    if (stack == NULL || --stack->refcount > 0)
        return;
    
    /* Free all chunks when refcount reaches 0 */
    tealet_chunk_t *chunk = stack->chunk.next;
    free(stack);
    while (chunk) {
        tealet_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
}
```

**Why reference counting?**

`tealet_duplicate()` creates a copy sharing the same saved stack:

```c
tealet_t *tealet_duplicate(tealet_t *tealet) {
    tealet_sub_t *copy = tealet_alloc(g_main);
    copy->stack_far = tealet->stack_far;
    copy->stack = tealet_stack_dup(tealet->stack);  /* Shared! */
    return (tealet_t*)copy;
}
```

Multiple tealets can share a stack snapshot, useful for the **stub pattern** (template tealets).

## The Switch Operation

### High-Level Flow

```c
int tealet_switch(tealet_t *target, void **parg) {
    g_main->g_target = target;
    g_main->g_arg = *parg;
    
    /* 1. Call stackman_switch() [platform-specific assembly] */
    stackman_switch(tealet_save_restore_cb, g_main);
    
    /* 2. Callback saves current stack */
    /* 3. Callback returns new stack pointer */
    /* 4. Assembly switches stack pointer */
    /* 5. Callback restores target stack */
    
    *parg = g_main->g_arg;
    return result;
}
```

### Save/Restore Callback State Machine

```c
typedef enum tealet_sr_e {
    SW_NOP,      /* No restore (save-only) */
    SW_RESTORE,  /* Restore target stack */
    SW_ERR,      /* Error during save */
} tealet_sr_e;
```

The callback is invoked **twice** by `stackman_switch()`:

**Call 1: STACKMAN_OP_SAVE**
- Save current stack
- Walk `g_prev` chain, growing stacks as needed
- Return new stack pointer (or current if no restore)

**Call 2: STACKMAN_OP_RESTORE**
- Restore target stack from heap to C stack
- Update state

## Memory Management

All allocations go through `tealet_alloc_t`:

```c
typedef struct tealet_alloc_t {
    tealet_malloc_t malloc_p;
    tealet_free_t free_p;
    void *context;
} tealet_alloc_t;
```

This allows:
- Custom allocators (memory pools, arenas)
- Instrumentation (leak detection, profiling)
- Platform-specific allocation strategies

Example with statistics:

```c
tealet_statsalloc_t stats_alloc;
tealet_alloc_t base = TEALET_ALLOC_INIT_MALLOC;
tealet_statsalloc_init(&stats_alloc, &base);

tealet_t *main = tealet_initialize(&stats_alloc.alloc, 0);
/* ... use tealets ... */
printf("Allocated: %zu bytes in %zu calls\n",
       stats_alloc.s_allocs, stats_alloc.n_allocs);
```

## Platform Abstraction

Platform-specific code is isolated in the **stackman** library:

```c
void *stackman_switch(stackman_cb_t callback, void *context);
```

stackman handles:
- Register saving/restoring
- Stack pointer manipulation
- Calling conventions (CDECL, STDCALL, etc.)
- Architecture differences (x86, ARM, RISC-V, etc.)

libtealet provides the **policy** (when/what to save), stackman provides the **mechanism** (how to switch).

## Performance Characteristics

### Time Complexity

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| `tealet_create()` | O(1) | Just allocates structure |
| `tealet_switch()` | O(n) | n = number of chunks to restore |
| `tealet_duplicate()` | O(1) | Reference count increment |
| Stack growth | O(k) | k = bytes to add |

### Space Complexity

**Per tealet:**
- Structure: ~64-128 bytes (platform-dependent)
- Saved stack: Variable, proportional to actual stack usage
- Extra data: User-specified

**Chunk overhead:**
- ~24 bytes per chunk
- Typical: 1-3 chunks per tealet

### Optimization: Stack Chaining Benefit

Consider 10 nested tealets at different stack depths:

**Without chaining (naive):**
- Each tealet saves its entire stack: 10 × stack_size

**With chaining:**
- Each tealet saves only the differential: sum of differentials ≈ stack_size

**Memory savings:** ~10× in nested scenarios

## Thread Safety

libtealet is **not thread-safe** across threads.

**Rules:**
1. Each thread has its own `main` tealet
2. Tealets with different `main` cannot interact
3. No synchronization primitives included

**Rationale:** Co-routines are user-space scheduling, not parallelism. For parallel execution, use OS threads + libtealet within each thread.

## Debugging

### Debug Builds

Compile with `-UNDEBUG` to enable assertions:

```bash
CFLAGS=-UNDEBUG make
```

Assertions check:
- Stack state consistency
- Reference count validity
- Chain integrity
- Switch preconditions

### Tealet IDs

In debug builds, each tealet gets an ID:

```c
#ifndef NDEBUG
tealet_sub_t->id = g_main->g_counter;
#endif
```

Useful for tracking which tealet is which during debugging.

### Common Issues

**Defunct tealet errors:**
- Cause: Memory allocation failure during stack save
- Fix: Check available memory, use smaller stacks

**Segfaults during switch:**
- Cause: Passing stack-allocated data between tealets
- Fix: Use `tealet_malloc()` for shared data

**Assertion failures in g_prev chain:**
- Cause: Corruption of chain pointers
- Fix: Check for buffer overruns, use valgrind

## Design Rationale

### Why Stack-Slicing vs. Separate Stacks?

**Separate stacks (like OS threads):**
- ✅ Simple: Each coroutine has fixed-size stack
- ❌ Memory waste: Pre-allocate large stacks
- ❌ Stack overflow: Fixed size can be too small

**Stack-slicing (libtealet):**
- ✅ Memory efficient: Grows on demand
- ✅ No stack overflow: Limited only by heap
- ❌ Complex: Requires saving/restoring

### Why Symmetric vs. Asymmetric Coroutines?

**Asymmetric (yield/resume):**
- ✅ Simpler mental model
- ✅ Natural for generators
- ❌ Limited: Can only yield to caller

**Symmetric (explicit switch):**
- ✅ Flexible: Switch to any coroutine
- ✅ Powerful: Build any scheduling strategy
- ❌ More complex: Must manage transitions

libtealet chooses symmetric for maximum flexibility.

### Why Reference Counting vs. Garbage Collection?

**Reference counting:**
- ✅ Deterministic: Resources freed immediately
- ✅ No runtime: No GC pauses
- ❌ Manual: User must avoid cycles

**Garbage collection:**
- ✅ Automatic: No manual management
- ❌ Pauses: Stop-the-world collection
- ❌ Non-deterministic: Unclear when freed

For a low-level C library, reference counting fits better.

## Further Reading

- [GETTING_STARTED.md](GETTING_STARTED.md) - Practical usage examples
- [API.md](API.md) - Complete function reference
- [stackman documentation](https://github.com/stackless-dev/stackman) - Low-level stack operations
- [Stackless Python](https://github.com/stackless-dev/stackless) - Original inspiration
- [Greenlet](https://greenlet.readthedocs.io/) - Related Python project
