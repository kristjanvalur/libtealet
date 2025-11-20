# API Reference

Complete reference for the libtealet API. All functions are declared in `tealet.h`.

## Table of Contents

- [Lifecycle Management](#lifecycle-management)
- [Coroutine Creation](#coroutine-creation)
- [Advanced: Fork-like Semantics](#advanced-fork-like-semantics)
- [Context Switching](#context-switching)
- [Status and Inspection](#status-and-inspection)
- [Memory Management](#memory-management)
- [Custom Allocators](#custom-allocators)
- [Error Codes](#error-codes)
- [Constants and Macros](#constants-and-macros)

## Lifecycle Management

### `tealet_initialize()`

```c
tealet_t *tealet_initialize(tealet_alloc_t *alloc, size_t extrasize);
```

Initialize the library and create the main tealet for the current thread.

**Parameters:**
- `alloc`: Pointer to allocator interface. Use `TEALET_ALLOC_INIT_MALLOC` for standard malloc/free.
- `extrasize`: Optional extra bytes allocated per tealet for user data. Access via `TEALET_EXTRA()` macro.

**Returns:** Pointer to the main tealet, or `NULL` on allocation failure.

**Usage:**
```c
tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
tealet_t *main = tealet_initialize(&alloc, 0);
if (main == NULL) {
    /* Out of memory */
}
```

Call once per thread before creating any tealets. The main tealet represents the initial execution context.

**Thread Safety:** Each thread must call `tealet_initialize()` separately.

---

### `tealet_finalize()`

```c
void tealet_finalize(tealet_t *main);
```

Clean up and destroy the main tealet.

**Parameters:**
- `main`: The main tealet returned by `tealet_initialize()`

**Usage:**
```c
tealet_finalize(main);
```

Call when done with all tealets in the current thread. This frees all resources associated with the main tealet. Do not call on non-main tealets.

⚠️ **Warning:** Ensure all child tealets are deleted or have exited before calling this.

---

## Coroutine Creation

### `tealet_create()`

```c
tealet_t *tealet_create(tealet_t *main, tealet_run_t run);
```

Create a new tealet without starting it.

**Parameters:**
- `main`: The main tealet (from `tealet_initialize()`)
- `run`: Function to execute in the tealet's context

**Returns:** New tealet in `TEALET_STATUS_INITIAL` state, or `NULL` on allocation failure.

**Usage:**
```c
tealet_t *t = tealet_create(main, my_run_function);
if (t == NULL) {
    /* Out of memory */
}
```

The tealet is created but not yet running. Use `tealet_switch()` to start execution. This pattern is useful when setting up multiple coroutines before starting any:

```c
tealet_t *t1 = tealet_create(main, func1);
tealet_t *t2 = tealet_create(main, func2);
/* Both created but not running yet */

void *arg1 = data1;
tealet_switch(t1, &arg1);  /* Start t1 */
```

**See Also:** `tealet_new()` for create-and-start in one call.

---

### `tealet_new()`

```c
tealet_t *tealet_new(tealet_t *main, tealet_run_t run, void **parg);
```

Create a new tealet and immediately start executing it.

**Parameters:**
- `main`: The main tealet
- `run`: Function to execute
- `parg`: Pointer to argument pointer (passed to run function, updated with return value)

**Returns:** New tealet (may be `NULL` if run function deleted it), or `NULL` on allocation failure.

**Usage:**
```c
void *arg = my_data;
tealet_t *t = tealet_new(main, my_run, &arg);
/* Run function has executed until first switch back */
/* arg now contains value from first switch */
```

Equivalent to:
```c
tealet_t *t = tealet_create(main, run);
if (t != NULL) {
    tealet_switch(t, parg);
}
```

Use when you want to start execution immediately without separate `tealet_switch()` call.

---

### Run Function Type

```c
typedef tealet_t *(*tealet_run_t)(tealet_t *current, void *arg);
```

Type of function executed by a tealet.

**Parameters:**
- `current`: The currently executing tealet (the one running this function)
- `arg`: Argument passed via `tealet_switch()` or `tealet_new()`

**Returns:** Next tealet to execute (typically `current->main`)

**Contract:**
- Must return a valid tealet pointer
- Typically return `current->main` to go back to caller
- Can return any other tealet to chain execution
- Returning `NULL` makes the tealet defunct

**Lifecycle:**
```c
tealet_t *my_run(tealet_t *current, void *arg) {
    /* Initialization */
    
    /* Do work, possibly switching to other tealets */
    void *data = process(arg);
    tealet_switch(current->main, &data);
    
    /* More work */
    
    /* Exit by returning */
    return current->main;
}
```

The run function executes until it returns or calls `tealet_exit()`. Upon return, the tealet is automatically deleted and execution transfers to the returned tealet.

---

## Advanced: Fork-like Semantics

⚠️ **Advanced Feature:** Fork-like semantics break the traditional function-scope discipline. Use with caution.

### `tealet_set_far()`

```c
int tealet_set_far(tealet_t *tealet, void *far_boundary);
```

Set a stack boundary on a tealet, limiting how far its stack can extend.

**Parameters:**
- `tealet`: The tealet to set boundary on (typically main)
- `far_boundary`: Pointer to a stack variable establishing the far boundary

**Returns:**
- `0` on success
- `-1` if called from non-main tealet or if tealet is not currently active

**Usage:**
```c
int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    int stack_marker;  /* Stack variable for boundary */
    tealet_set_far(main, &stack_marker);
    
    /* Now main has a bounded stack */
    /* Can fork or perform other operations requiring bounded stacks */
    
    tealet_finalize(main);
    return 0;
}
```

**Why set boundaries?**
- **Required before forking:** Prevents creating two unbounded stacks
- **Memory control:** Limits stack growth for specific tealets
- **Scope discipline:** Ensures execution stays within promised bounds

**Current limitation:** Can only be called on the main tealet.

---

### `tealet_fork()`

```c
int tealet_fork(tealet_t *current, tealet_t **pother, int flags);
```

Fork the current tealet, creating a child tealet that duplicates the execution state.

**Parameters:**
- `current`: The currently active tealet to fork
- `pother`: Pointer to receive the "other" tealet pointer:
  - In parent: receives pointer to child
  - In child: receives pointer to parent
- `flags`: Fork mode flags:
  - `TEALET_FORK_DEFAULT` (0): Child created suspended, parent continues
  - `TEALET_FORK_SWITCH` (1): Immediately switch to child after creation

**Returns:**
- Parent: `1` (FORK_DEFAULT) - you are the parent
- Child: `0` - you are the child  
- Error: negative error code
  - `TEALET_ERR_UNFORKABLE`: Current tealet has unbounded stack (call `tealet_set_far()` first)
  - `TEALET_ERR_MEM`: Memory allocation failed

**Usage:**
```c
int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* REQUIRED: Set stack boundary before forking */
    int stack_marker;
    tealet_set_far(main, &stack_marker);
    
    tealet_t *other = NULL;
    int result = tealet_fork(main, &other, TEALET_FORK_DEFAULT);
    
    if (result == 0) {
        /* This is the CHILD */
        printf("Child: parent is %p\n", other);
        
        /* CRITICAL: Forked tealets MUST use tealet_exit() */
        tealet_exit(other, NULL, 0);  /* Switch back to parent */
        
        /* Should not reach here */
        abort();
        
    } else if (result > 0) {
        /* This is the PARENT */
        printf("Parent: child is %p\n", other);
        
        /* Switch to child when ready */
        tealet_switch(other, NULL);
        
        /* Clean up */
        tealet_delete(other);
    } else {
        /* Error occurred */
        fprintf(stderr, "Fork failed: %d\n", result);
    }
    
    tealet_finalize(main);
    return 0;
}
```

**Critical responsibilities:**

1. **Set stack boundary first:** Must call `tealet_set_far()` before forking to avoid unbounded stacks
2. **Forked tealets must use `tealet_exit()`:** Unlike tealets created with `tealet_new()`, forked tealets have no run function. Simply returning from the fork point is undefined behavior.
3. **No DEFER flag:** Forked tealets must not use `TEALET_EXIT_DEFER` when exiting
4. **Stay within bounds:** All switching must occur within the stack region bounded by `far_boundary`

**Philosophical note:**

Traditional tealet creation (`tealet_new()`, `tealet_create()`) maintains clean function-scope discipline - each tealet exists within a specific function's execution. `tealet_fork()` breaks this discipline, enabling dynamic coroutine cloning at any point. This power comes with responsibility: you must manually manage stack boundaries and explicit exit.

This feature mirrors functionality from Stackless Python but was historically omitted from libtealet to keep the API simple and safe. Use it when you need advanced patterns like continuation capture or coroutine cloning.

**Flags:**
- `TEALET_FORK_DEFAULT`: Child suspended, parent continues (Unix fork-like)
- `TEALET_FORK_SWITCH`: Immediately become the child, parent suspended

---

## Context Switching

### `tealet_switch()`

```c
int tealet_switch(tealet_t *target, void **parg);
```

Suspend current tealet and resume target tealet.

**Parameters:**
- `target`: Tealet to switch to
- `parg`: Pointer to argument pointer (passed to target, updated with return value)

**Returns:** 
- `0` on success
- `TEALET_ERR_DEFUNCT` if target is corrupt
- Negative error code on failure

**Usage:**
```c
void *arg = my_data;
int result = tealet_switch(target, &arg);
if (result == 0) {
    /* arg now contains value passed back from target */
} else {
    /* Handle error */
}
```

**Behavior:**
1. Saves current stack state
2. Passes `*parg` to target
3. Resumes target execution
4. When target switches back, updates `*parg` with return value

**Example - Ping Pong:**
```c
tealet_t *ping(tealet_t *current, void *arg) {
    tealet_t *pong = (tealet_t*)arg;
    
    void *data = "ping";
    tealet_switch(pong, &data);
    /* data now contains "pong" */
    
    return current->main;
}

tealet_t *pong(tealet_t *current, void *arg) {
    void *data = "pong";
    tealet_switch(arg, &data);  /* Switch back to ping */
    return current->main;
}
```

⚠️ **Stack Safety:** Never pass pointers to stack-allocated data in `parg`. The stack becomes invalid when switching. Allocate shared data on the heap.

---

### `tealet_exit()`

```c
void tealet_exit(tealet_t *target, void **parg, int flags);
```

Exit current tealet and transfer control to target.

**Parameters:**
- `target`: Tealet to switch to
- `parg`: Argument to pass to target
- `flags`: Control flags (see below)

**Flags:**
- `TEALET_EXIT_DEFAULT` (0): Don't auto-delete, manual cleanup required
- `TEALET_EXIT_DELETE`: Auto-delete tealet on exit (same as return behavior)
- `TEALET_EXIT_DEFER`:  Store flags and argument, return to caller.  Will be used when
  tealet exits later.

**Usage:**
```c
tealet_t *my_run(tealet_t *current, void *arg) {
    /* Do work */
    
    /* Exit and delete this tealet */
    tealet_exit(current->main, &result, TEALET_EXIT_DELETE);
    
    /* Never reached */
}
```

**With TEALET_EXIT_DEFER:**
```c
tealet_t *my_run(tealet_t *current, void *arg) {
    if (should_exit) {
        /* store exit target and flags /*
        tealet_exit(current->main, &result, TEALET_EXIT_DEFER);
        /* Returns here, can do cleanup */
        cleanup_resources();
    }
    
    /* return value ignored.
     * exit value from earlier will be used.
     * Tealet is not auto-deleted
     */
    return nullptr;
}
```

This function does not return unless `TEALET_EXIT_DEFER` is set.

**Note:** Returning from the run function automatically deletes the tealet. Use `tealet_exit()` when you need explicit control over deletion or want to exit from nested calls within the run function.

---

## Status and Inspection

### `tealet_status()`

```c
int tealet_status(tealet_t *t);
```

Get the current status of a tealet.

**Parameters:**
- `t`: Tealet to inspect

**Returns:** Status code:
- `TEALET_STATUS_INITIAL` (0): Created but not yet started
- `TEALET_STATUS_ACTIVE` (1): Started and can be switched to
- `TEALET_STATUS_DEFUNCT` (-1): Corrupt or returned from run function

**Usage:**
```c
int status = tealet_status(t);
switch (status) {
    case TEALET_STATUS_INITIAL:
        /* Can switch to it for first time */
        break;
    case TEALET_STATUS_ACTIVE:
        /* Can switch to it */
        break;
    case TEALET_STATUS_DEFUNCT:
        /* Cannot use, should delete */
        tealet_delete(t);
        break;
}
```

**Generator Pattern:**
```c
while (tealet_status(gen) == TEALET_STATUS_ACTIVE) {
    process(value);
    tealet_switch(gen, &value);  /* Get next value */
}
```

---

### `TEALET_IS_MAIN()`

```c
int TEALET_IS_MAIN(tealet_t *t);
```

Check if a tealet is the main tealet.

**Returns:** Non-zero if `t` is the main tealet, zero otherwise.

**Usage:**
```c
if (TEALET_IS_MAIN(current)) {
    /* This is the main tealet */
}
```

---

### `TEALET_MAIN()`

```c
tealet_t *TEALET_MAIN(tealet_t *t);
```

Get the main tealet associated with a tealet.

**Returns:** Pointer to the main tealet.

**Usage:**
```c
tealet_t *main = TEALET_MAIN(current);
```

Equivalent to `t->main` but works correctly for both main and non-main tealets.

---

### `TEALET_EXTRA()`

```c
void *TEALET_EXTRA(tealet_t *t);
```

Get pointer to extra user data in a tealet.

**Returns:** Pointer to extra data buffer, or `NULL` if `extrasize` was 0.

**Usage:**
```c
/* Initialize with extra space */
tealet_t *main = tealet_initialize(&alloc, sizeof(my_context_t));

/* Access in run function */
tealet_t *my_run(tealet_t *current, void *arg) {
    my_context_t *ctx = (my_context_t*)TEALET_EXTRA(current);
    ctx->counter++;
    /* ... */
    return current->main;
}
```

The extra data is allocated with the tealet and freed when the tealet is deleted. Useful for per-tealet state without separate allocations.

---

## Memory Management

### `tealet_delete()`

```c
void tealet_delete(tealet_t *t);
```

Explicitly delete a tealet and free its resources.

**Parameters:**
- `t`: Tealet to delete (must not be currently executing)

**Usage:**
```c
tealet_t *t = tealet_create(main, my_run);
/* Use t... */
tealet_delete(t);
```

**Important:**
- Cannot delete the currently executing tealet (use `tealet_exit()` instead)
- Cannot delete the main tealet (use `tealet_finalize()`)
- Tealets are automatically deleted when their run function returns
- Only call on tealets you've kept alive explicitly

**When to Use:**
- Deleting tealets that never ran (`TEALET_STATUS_INITIAL`)
- Cleaning up tealets kept alive with `TEALET_EXIT_DEFAULT`
- Manual resource management

**When Not Needed:**
- Run function returns normally → automatic deletion
- `tealet_exit()` with `TEALET_EXIT_DELETE` → automatic deletion

---

## Custom Allocators

### `tealet_alloc_t`

```c
typedef struct tealet_alloc {
    void *context;
    tealet_malloc_t malloc_func;
    tealet_free_t free_func;
} tealet_alloc_t;
```

Custom memory allocator interface.

**Fields:**
- `context`: User-defined allocator state (passed to malloc/free functions)
- `malloc_func`: Allocation function
- `free_func`: Deallocation function

**Function Types:**
```c
typedef void *(*tealet_malloc_t)(void *context, size_t size);
typedef void (*tealet_free_t)(void *context, void *ptr);
```

**Usage - Standard Allocator:**
```c
tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
tealet_t *main = tealet_initialize(&alloc, 0);
```

**Usage - Custom Allocator:**
```c
void *my_malloc(void *context, size_t size) {
    memory_pool_t *pool = (memory_pool_t*)context;
    return pool_allocate(pool, size);
}

void my_free(void *context, void *ptr) {
    memory_pool_t *pool = (memory_pool_t*)context;
    pool_free(pool, ptr);
}

memory_pool_t pool;
init_pool(&pool, 1024 * 1024);

tealet_alloc_t alloc = {
    .context = &pool,
    .malloc_func = my_malloc,
    .free_func = my_free
};

tealet_t *main = tealet_initialize(&alloc, 0);
```

All internal allocations (tealet structures, stack storage) use this interface. The allocator must remain valid for the lifetime of all tealets.

---

### `TEALET_ALLOC_INIT_MALLOC`

```c
#define TEALET_ALLOC_INIT_MALLOC { NULL, tealet_malloc_malloc, tealet_free_free }
```

Initializer for standard malloc/free allocator.

**Usage:**
```c
tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
```

---

## Error Codes

### `TEALET_ERR_MEM`

```c
#define TEALET_ERR_MEM (-1)
```

Memory allocation failed.

**Recovery:**
- Free unused resources
- Reduce memory usage
- Fail gracefully

**Example:**
```c
tealet_t *t = tealet_create(main, my_run);
if (t == NULL) {
    fprintf(stderr, "Out of memory\n");
    return TEALET_ERR_MEM;
}
```

---

### `TEALET_ERR_DEFUNCT`

```c
#define TEALET_ERR_DEFUNCT (-2)
```

Target tealet is defunct (corrupt or completed).

**Causes:**
- Run function returned (normal completion)
- Run function returned `NULL` (error)
- Internal corruption

**Recovery:**
- Check `tealet_status()` before switching
- Delete defunct tealets
- Don't reuse completed tealets

**Example:**
```c
int result = tealet_switch(t, &arg);
if (result == TEALET_ERR_DEFUNCT) {
    fprintf(stderr, "Tealet has exited\n");
    tealet_delete(t);
    return -1;
}
```

---

## Constants and Macros

### Status Codes

```c
#define TEALET_STATUS_INITIAL  0   /* Created, not started */
#define TEALET_STATUS_ACTIVE   1   /* Running, can switch to */
#define TEALET_STATUS_DEFUNCT  (-1) /* Completed or corrupt */
```

---

### Flags

```c
/* Exit flags (new names) */
#define TEALET_EXIT_DEFAULT 0  /* Don't auto-delete */
#define TEALET_EXIT_DELETE  1  /* Auto-delete on exit */
#define TEALET_EXIT_DEFER   2  /* Defer exit to return */
```

Used with `tealet_exit()`.

---

## Decision Trees

### When to Use `tealet_create()` vs `tealet_new()`

**Use `tealet_create()`:**
- Setting up multiple coroutines before starting any
- Need to pass tealet pointer to other setup code
- Separating creation from execution for clarity

**Use `tealet_new()`:**
- Start execution immediately
- Don't need the tealet pointer before it runs
- More concise code

**Example Comparison:**
```c
/* Pattern 1: Create then switch (more control) */
tealet_t *t1 = tealet_create(main, func1);
tealet_t *t2 = tealet_create(main, func2);
setup_relationship(t1, t2);  /* Both exist but not started */
void *arg = t2;
tealet_switch(t1, &arg);  /* Now start t1 */

/* Pattern 2: Create and start (more concise) */
void *arg = my_data;
tealet_t *t = tealet_new(main, my_func, &arg);
/* Already started, arg contains first return value */
```

---

### When to Use `tealet_exit()` vs Returning

**Return from run function:**
- Normal completion
- Simplest code path
- Automatic cleanup (tealet is deleted)
- Can only specify exit target tealet, no flags.

**Use `tealet_exit()` with `TEALET_EXIT_DELETE`:**
- Need to exit from nested function calls
- Want explicit exit point
- Conditional exit logic

**Use `tealet_exit()` with `TEALET_EXIT_DEFER`:**
- Want to signal exit target and set exit flags.
- Need to do additional cleanup afterwards, e.g. run destructors.

**Examples:**
```c
/* Simple: Just return */
tealet_t *simple_run(tealet_t *current, void *arg) {
    process(arg);
    return current->main;  /* Done */
}

/* Nested exit */
tealet_t *nested_run(tealet_t *current, void *arg) {
    helper(current);  /* May call tealet_exit() */
    return current->main;
}

void helper(tealet_t *current) {
    if (error_condition) {
        void *error = make_error();
        tealet_exit(current->main, &error, TEALET_EXIT_DELETE);
        /* Never returns */
    }
}

/* Cleanup after exit */
tealet_t *cleanup_run(tealet_t *current, void *arg) {
    FILE *f = open_file();
    
    if (should_exit) {
        tealet_exit(current->main, &result, TEALET_EXIT_DEFER);
        /* Returns here */
        fclose(f);  /* Cleanup still runs */
    }
    
    fclose(f);
    return current->main;
}
```

---

## Stub Pattern

For tealets that act as targets without their own run logic:

```c
tealet_t *stub_run(tealet_t *current, void *arg) {
    /* This tealet is just a target, it never actively runs */
    return current->main;
}

tealet_t *stub = tealet_create(main, stub_run);
/* Use stub as a target for tealet_switch() from other tealets */
```

Useful for synchronization points or placeholders in complex coroutine networks.

---

## Complete Example

```c
#include <stdio.h>
#include <stdlib.h>
#include "tealet.h"

typedef struct {
    int max;
    int current;
} counter_state_t;

tealet_t *counter_run(tealet_t *current, void *arg) {
    counter_state_t *state = (counter_state_t*)TEALET_EXTRA(current);
    state->current = 0;
    state->max = *(int*)arg;
    
    while (state->current < state->max) {
        void *value = (void*)(intptr_t)state->current;
        state->current++;
        tealet_switch(current->main, &value);
    }
    
    return current->main;
}

int main(void) {
    /* Initialize with extra space for state */
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, sizeof(counter_state_t));
    if (main == NULL) {
        fprintf(stderr, "Failed to initialize\n");
        return 1;
    }
    
    /* Create counter tealet */
    tealet_t *counter = tealet_create(main, counter_run);
    if (counter == NULL) {
        fprintf(stderr, "Failed to create counter\n");
        tealet_finalize(main);
        return 1;
    }
    
    /* Start it */
    int max = 5;
    void *arg = &max;
    int result = tealet_switch(counter, &arg);
    if (result != 0) {
        fprintf(stderr, "Switch failed: %d\n", result);
        tealet_delete(counter);
        tealet_finalize(main);
        return 1;
    }
    
    /* Pull values */
    while (tealet_status(counter) == TEALET_STATUS_ACTIVE) {
        int value = (int)(intptr_t)arg;
        printf("Value: %d\n", value);
        
        result = tealet_switch(counter, &arg);
        if (result != 0) {
            fprintf(stderr, "Switch failed: %d\n", result);
            break;
        }
    }
    
    /* Cleanup */
    if (tealet_status(counter) == TEALET_STATUS_DEFUNCT) {
        tealet_delete(counter);
    }
    tealet_finalize(main);
    
    return 0;
}
```

Output:
```
Value: 0
Value: 1
Value: 2
Value: 3
Value: 4
```
