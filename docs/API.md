# API Reference

Complete reference for the libtealet API. Core functions are declared in `tealet.h`; helper extensions are declared in `tealet_extras.h`.

## Table of Contents

- [Lifecycle Management](#lifecycle-management)
- [Thread Safety and Locking Model](#thread-safety-and-locking-model)
- [Coroutine Creation](#coroutine-creation)
- [Advanced: Fork-like Semantics](#advanced-fork-like-semantics)
- [Context Switching](#context-switching)
- [Status and Inspection](#status-and-inspection)
- [Stack Utilities](#stack-utilities)
- [Memory Management](#memory-management)
- [Custom Allocators](#custom-allocators)
- [Helper Extensions (`tealet_extras.h`)](#helper-extensions-tealet_extrash)
- [Error handling](#error-handling)
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

Call when done with all tealets in the current thread. This destroys the main tealet itself. Do not call on non-main tealets.

⚠️ **Warning:** `tealet_finalize()` does **not** walk and delete child tealets. Delete non-main tealets before finalizing. After finalize returns, all tealet handles from that main tealet are invalid and must not be used (including `tealet_delete()` and `tealet_free()`).

There is no supported way to decouple this deletion order: tealet API operations rely on allocator/context state stored in the main tealet, and that state is destroyed by `tealet_finalize()`.

---

## Thread Safety and Locking Model

libtealet uses a mixed thread model:

- Switching behavior is thread-affine for a main-tealet domain.
- Structure operations may be synchronized across threads.

In practice, this means you may safely coordinate foreign-thread operations
such as `tealet_delete()` / `tealet_duplicate()` when synchronization is
correctly applied, while still treating switching itself as an owning-thread
operation.

### Automatic vs manual locking

Locking mode is configured via `tealet_config_set_locking()` and
`tealet_lock_t.mode`:

- `TEALET_LOCK_SWITCH`: libtealet automatically acquires/releases the lock for
    switching APIs (`tealet_new()`, `tealet_create()`, `tealet_switch()`,
    `tealet_exit()`, `tealet_fork()`).
- `TEALET_LOCK_OFF`: no internal auto-locking; integrator fully controls lock
    scopes.

### Manual synchronization rules

Use manual synchronization when:

- non-switch APIs may be called from foreign threads,
- you need consistency across multiple API calls (for example, read
    `tealet_previous()` or `tealet_current()`, then use that pointer in a later
    API call).

Preferred practice is to use `tealet_lock()` / `tealet_unlock()` around the
entire sequence so lock scopes stay compatible with switching auto-lock mode.

Automatic locking in `TEALET_LOCK_SWITCH` mode does not require recursive lock
primitives. Integrators may intentionally assert non-recursion in callbacks to
enforce correct API usage.

### Entry-function discipline (manual locking mode)

This discipline is required when using manual lock scopes (`TEALET_LOCK_OFF` or
equivalent integration-managed locking):

1. On entry to a tealet run function, release the lock.
2. Execute run-body logic unlocked.
3. Reacquire before returning.
4. Reacquire before any `tealet_exit()` call from the run body.

In `TEALET_LOCK_SWITCH` mode, libtealet maintains this entry/exit discipline
internally for switching transitions.

---

## Coroutine Creation

### `tealet_create()`

```c
tealet_t *tealet_create(tealet_t *main, tealet_run_t run, void *stack_far);
```

Create a new tealet without starting it.

**Parameters:**
- `main`: The main tealet (from `tealet_initialize()`)
- `run`: Function to execute in the tealet's context
- `stack_far`: Optional far-boundary requirement for the initial stack snapshot (`NULL` uses default). This acts as a minimum boundary: it can only extend capture range, never shrink it.

**Returns:** New tealet in `TEALET_STATUS_INITIAL` state, or `NULL` on allocation failure.

**Usage:**
```c
tealet_t *t = tealet_create(main, my_run_function, NULL);
if (t == NULL) {
    /* Out of memory */
}
```

The tealet is created but not yet running. Use `tealet_switch()` to start execution. This pattern is useful when setting up multiple coroutines before starting any:

```c
tealet_t *t1 = tealet_create(main, func1, NULL);
tealet_t *t2 = tealet_create(main, func2, NULL);
/* Both created but not running yet */

void *arg1 = data1;
tealet_switch(t1, &arg1);  /* Start t1 */
```

**See Also:** `tealet_new()` for create-and-start in one call.

---

### `tealet_new()`

```c
tealet_t *tealet_new(tealet_t *main, tealet_run_t run, void **parg, void *stack_far);
```

Create a new tealet and immediately start executing it.

**Parameters:**
- `main`: The main tealet
- `run`: Function to execute
- `parg`: Pointer to argument pointer (passed to run function, updated with return value)
- `stack_far`: Optional far-boundary requirement for the initial stack snapshot (`NULL` uses default). This acts as a minimum boundary: it can only extend capture range, never shrink it.

**Returns:** New tealet (may be `NULL` if run function deleted it), or `NULL` on failure.

Conceptually, `tealet_new()` is `tealet_create()` followed by `tealet_switch()` (create-and-start in one call).

`tealet_new()` performs an internal switch as part of create-and-start. The same switch failure conditions as `tealet_switch()` apply (`TEALET_ERR_MEM`, `TEALET_ERR_DEFUNCT`, `TEALET_ERR_PANIC`), but `tealet_new()` reports them as `NULL` rather than returning an error code.

**Usage:**
```c
void *arg = my_data;
tealet_t *t = tealet_new(main, my_run, &arg, NULL);
/* Run function has executed until first switch back */
/* arg now contains value from first switch */
```

Equivalent to:
```c
tealet_t *t = tealet_create(main, run, NULL);
if (t != NULL) {
    tealet_switch(t, parg);
}
```

Use when you want to start execution immediately without separate `tealet_switch()` call.

### Example: include additional caller stack data

This capability exists primarily to make libtealet work with existing code where a new tealet needs to access stack-based structures from the creator path. Use a boundary requirement from a higher frame when you need the new tealet's initial snapshot to include locals from the creator function.

```c
typedef struct {
    int a;
    int b;
} local_state_t;

tealet_t *my_run(tealet_t *current, void *arg) {
    local_state_t *state = (local_state_t *)arg;
    /* state->a and state->b are available in the tealet context */
    return current->main;
}

void bar(tealet_t *main) {
    local_state_t local_state;
    void *arg;
    void *stack_far;
    tealet_t *worker;

    local_state.a = 1;
    local_state.b = 2;
    arg = &local_state;
    stack_far = tealet_stack_further(&local_state, &local_state + 1);

    worker = tealet_new(main, my_run, &arg, stack_far);
    (void)worker;
}
```

Using `tealet_stack_further(&local_state, &local_state + 1)` sets a direction-aware struct-granularity boundary so the full `local_state` object is within the new tealet's initial snapshot.

Boundary semantics are direction-dependent:
- descending stacks: boundary is exclusive for saved bytes
- ascending stacks: boundary is inclusive for saved bytes

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

**Best Practice - Use Parent Function's Stack Variable:**

The `far_boundary` should typically be a local variable from a **parent function** (caller) of the function that will perform fork operations. This ensures all local variables in the forking function are included in the saved stack.

**Recommended pattern:**
```c
int main(void) {
    int far_marker;  /* Boundary marker in parent function */
    run_program(&far_marker);
    return 0;
}

void run_program(void *far_marker) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    tealet_set_far(main, far_marker);
    
    int local_data = 0;  /* This WILL be saved when forking */
    tealet_fork(main, &child, NULL, 0);
    /* local_data and other locals are safely in the saved stack */
    
    tealet_finalize(main);
}
```

**Alternative - Same function (requires care):**
```c
void my_main(void) {
    int far_marker;  /* MUST be declared FIRST */
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    tealet_set_far(main, &far_marker);
    
    int local_data = 0;  /* Declared AFTER far_marker */
    tealet_fork(main, &child, NULL, 0);
    /* local_data is safely below far_marker on stack */
}
```

**Why set boundaries?**
- **Required for main-lineage forks:** Prevents creating two unbounded stacks when forking main (or clones of main)
- **Memory control:** Limits stack growth for specific tealets
- **Scope discipline:** Ensures execution stays within promised bounds

You can also obtain a call-site boundary marker with `tealet_new_probe()` when you need to inspect boundary requirements at a specific stack depth.

**Current limitation:** Can only be called for the main tealet.

---

### `tealet_fork()`

```c
int tealet_fork(tealet_t *current, tealet_t **pother, void **parg, int flags);
```

Fork the current tealet, creating a child tealet that duplicates the execution state.

**Parameters:**
- `current`: The currently active tealet to fork
- `pother`: Pointer to receive the "other" tealet pointer:
  - In parent: receives pointer to child
  - In child: receives pointer to parent
- `parg`: Pointer to argument pointer for passing values to the suspended side. Can be NULL if no argument passing is desired (similar to `tealet_new()` and `tealet_switch()`)
  - With `TEALET_FORK_DEFAULT`: Parent continues, child is suspended. When parent switches to child, child receives value via `*parg`
  - With `TEALET_FORK_SWITCH`: Child continues, parent is suspended. When child switches back, parent receives value via `*parg`
  - See `tealet_new()` for detailed argument passing semantics
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
    void *arg = NULL;
    int result = tealet_fork(main, &other, &arg, TEALET_FORK_DEFAULT);
    
    if (result == 0) {
        /* This is the CHILD */
        printf("Child: parent is %p, received arg=%p\n", other, arg);
        
        /* CRITICAL: Forked tealets MUST use tealet_exit() */
        void *return_value = (void*)0x1234;
        tealet_exit(other, return_value, 0);  /* Switch back to parent */
        
        /* Should not reach here */
        abort();
        
    } else if (result > 0) {
        /* This is the PARENT */
        printf("Parent: child is %p\n", other);
        
        /* Switch to child with an argument */
        arg = (void*)0x5678;
        tealet_switch(other, &arg);
        printf("Parent: child returned with arg=%p\n", arg);
        
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

1. **Forking main-lineage execution requires a boundary:** When `current` is the main tealet (or a clone of main), call `tealet_set_far()` first to avoid unbounded-stack forks.
2. **Far boundary is inherited by the child:** The forked child copies the
    parent's current far boundary at fork time.
3. **Exit style depends on what was forked:**
    - Main-lineage forks should exit via `tealet_exit()` with an explicit target.
    - Forks of regular function-scoped tealets can generally return through the same run-function path as the original tealet.
4. **No DEFER for main-lineage forks:** Do not use `TEALET_EXIT_DEFER` in that mode.
5. **Stay within bounds:** All switching must occur within the stack region bounded by `far_boundary` where a boundary is in effect.

**Philosophical note:**

Traditional tealet creation (`tealet_new()`, `tealet_create()`) maintains clean function-scope discipline - each tealet exists within a specific function's execution. `tealet_fork()` can break this discipline when used on main-lineage execution, but behaves like a direct continuation clone when used inside a regular function-scoped tealet.

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
- `TEALET_ERR_MEM` on memory allocation failure while saving/restoring state
- `TEALET_ERR_DEFUNCT` if target is defunct
- `TEALET_ERR_PANIC` on main when control was rerouted to main from `tealet_exit()` because the requested exit target was defunct
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

**Exception:** If you intentionally expand the new tealet's initial saved stack via `stack_far` (as shown in the `tealet_new()` example above), stack-based structures in that expanded region can be valid for that tealet's execution.

---

### `tealet_exit()`

```c
int tealet_exit(tealet_t *target, void *arg, int flags);
```

Exit current tealet and transfer control to target.

**Parameters:**
- `target`: Tealet to switch to
- `arg`: Argument to pass to target
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

**Returns:**
- `0` only when `TEALET_EXIT_DEFER` is used (deferred exit setup succeeds)
- Negative error code on failure
    - May include `TEALET_ERR_MEM` and `TEALET_ERR_DEFUNCT`

When exit to a non-main target fails, the implementation falls back to exiting to `main`.

If the chosen exit target is defunct, this fallback path reroutes the exit to `main`.

**Note:** Returning from the run function automatically deletes the tealet. Use `tealet_exit()` when you need explicit control over deletion or want to exit from nested calls within the run function.

> Future direction: a dedicated panic API may be added (for example, `tealet_panic()`), either forcing switch-to-main with a panic flag or returning a dedicated panic error code for corrupt stack-state scenarios.

---

## Status and Inspection

Query APIs in this section are not internally synchronized.

If they are used from foreign threads, or concurrently with switching/lifecycle
operations, callers must provide external synchronization (for example
`tealet_lock()`/`tealet_unlock()` around the access sequence).

### `tealet_previous()`

```c
tealet_t *tealet_previous(tealet_t *tealet);
```

Get the tealet that most recently switched to the current tealet.

**Parameters:**
- `tealet`: Any tealet derived from the same main tealet

**Returns:**
- Pointer to the previous tealet when available
- `NULL` when no previous tealet is recorded
- `NULL` when the previously recorded tealet has been destroyed

**Note:** In a multithreaded setting, a previous tealet can be simultaneously
deleted by a different thread, invalidating the returned pointer.

**Usage:**
```c
tealet_t *prev = tealet_previous(current);
if (prev == NULL) {
    /* No valid previous tealet */
}
```

---

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

### `tealet_get_origin()`

```c
unsigned int tealet_get_origin(tealet_t *tealet);
```

Get origin flags that describe tealet origin/lineage.

**Returns:** Bitwise OR of:

- `TEALET_ORIGIN_MAIN_LINEAGE`: Main tealet, or fork-descended from main-lineage
- `TEALET_ORIGIN_FORK`: Tealet originated via `tealet_fork()`

**Usage:**

```c
unsigned int origin = tealet_get_origin(current);
if (origin & TEALET_ORIGIN_MAIN_LINEAGE) {
    /* main-like fork caveats apply */
}
if (origin & TEALET_ORIGIN_FORK) {
    /* current tealet was produced by tealet_fork() */
}
```

Convenience macros are also available:

- `TEALET_IS_MAIN_LINEAGE(t)`
- `TEALET_IS_FORK(t)`

---

### `tealet_main_userpointer()`

```c
void **tealet_main_userpointer(tealet_t *tealet);
```

Get the address of the per-main user pointer slot.

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

## Stack Utilities

### `tealet_stack_diff()`

```c
ptrdiff_t tealet_stack_diff(void *a, void *b);
```

Compute stack-relative distance between two addresses in a direction-aware way.

**Returns:** Positive when `b` is deeper on stack than `a`, zero if equal, negative otherwise.

---

### `tealet_stack_further()`

```c
void *tealet_stack_further(void *a, void *b);
```

Return whichever stack position is farther from the active stack top.

This is direction-aware:
- descending stacks: returns the greater address
- ascending stacks: returns the smaller address

Use this to combine boundary requirements from different stack references.

---

### `tealet_new_probe()`

```c
void *tealet_new_probe(tealet_t *dummy1, tealet_run_t dummy2, void **dummy3, void *dummy4);
```

A function to test what stack boundary would be used for a new tealet without creating one. This is mostly informative.

**Parameters:**
- `dummy1`, `dummy2`, `dummy3`: Unused placeholders to match the `tealet_new()` call shape
- `dummy4`: Optional far-boundary requirement; if provided, it is clamped so it can only extend (not shrink) the probe boundary

**Returns:** The probed stack-boundary marker.

**Usage:**
```c
void *arg = NULL;
void *probe = tealet_new_probe(main, my_run, &arg, NULL);
printf("new tealet boundary probe: %p\n", probe);
```

Use this when you want to inspect the effective boundary at a specific stack depth without creating a tealet. You do not need to pass this value to `tealet_new()`, which computes its own boundary using the same logic.

---

## Memory Management

### Runtime stack-check configuration

libtealet exposes a versioned configuration API for stack-integrity checks.

### Build-time availability and defaults

Feature support is compile-time gated:

- `TEALET_WITH_STACK_GUARD`
- `TEALET_WITH_STACK_SNAPSHOT`

In the default project build, both are enabled. You can compile either or both out.

When a feature is not available in the current build/platform, `tealet_configure_set()` canonicalizes the request to the supported subset.

### Guard vs snapshot

- `TEALET_CONFIGF_STACK_GUARD` protects full pages in the monitored window.
    - On Linux/Unix builds with page protection support, this is implemented with `mprotect()`.
    - Because page protection is page-granular, it cannot precisely cover arbitrary sub-page boundaries.
    - Guard application/removal is best-effort; if `mprotect()` cannot represent or apply an interval, libtealet clears active guard state for that cycle and continues.
- `TEALET_CONFIGF_STACK_SNAPSHOT` captures monitored bytes and verifies them on switch-back.
    - This is byte-granular and can cover sub-page regions that page protection cannot represent directly.
    - In `TEALET_STACK_INTEGRITY_FAIL_ERROR` mode, a mismatch returns `TEALET_ERR_INTEGRITY` and clears the active snapshot marker for that cycle so execution can continue.

In typical protected builds, libtealet combines both:

- guard catches direct accesses into guarded pages (hard faults),
- snapshot checks the remaining unguardable edge bytes (soft integrity failures under configured policy).

This combined mode gives broad coverage of out-of-bounds stack access while keeping behavior configurable by fail policy.

### `tealet_configure_get()`

```c
int tealet_configure_get(tealet_t *tealet, tealet_config_t *config);
```

Read current effective runtime configuration from a main tealet.

**Notes:**
- Uses `tealet_config_t` size/version semantics.
- Writes only the caller-provided struct prefix (`config->size`).
- Returned values are canonicalized effective values.

---

### `tealet_configure_set()`

```c
int tealet_configure_set(tealet_t *tealet, tealet_config_t *config);
```

Apply runtime configuration to a main tealet.

**Behavior:**
- Validates struct header/version.
- Canonicalizes unsupported/invalid combinations.
- Pre-allocates required snapshot workspace.
- Writes canonicalized effective config back to caller.

Use this for explicit tuning of flags, guard mode, integrity bytes, and fail policy.

`tealet_config_t` also includes `stack_guard_limit`:
- `NULL`: no explicit far-limit clamp.
- non-`NULL`: planning clamps monitored bytes so the protected interval does not extend past this address.  It is recommended to set this to a known address on the
stack to ensure that stack protection does not try to inspect bytes outside of the
process stack segment.

`tealet_config_t` also includes `max_stack_size`:
- default: `TEALET_DEFAULT_MAX_STACK_SIZE` (16 MiB)
- non-zero: upper bound used only by caller switch-sanity validation (`tealet_switch*` caller-context checks)
- `0`: disable this validation, removing stack-size assumptions from caller checks

The caller stack-distance check is measured relative to a stack probe captured for the main tealet during `tealet_initialize()`.

---

### `tealet_configure_check_stack()`

```c
int tealet_configure_check_stack(tealet_t *tealet, size_t stack_integrity_bytes);
```

Convenience helper to turn stack checks on with sensible defaults.

**What it enables:**
- `TEALET_CONFIGF_STACK_INTEGRITY`
- `TEALET_CONFIGF_STACK_GUARD`
- `TEALET_CONFIGF_STACK_SNAPSHOT`

If guard support is unavailable on the current build/platform, canonicalization leaves snapshot-only integrity enabled where supported.

**Default policy choices:**
- guard mode: `TEALET_STACK_GUARD_MODE_NOACCESS`
- fail policy: `TEALET_STACK_INTEGRITY_FAIL_ERROR`
- stack guard limit: address of a local variable inside `tealet_configure_check_stack()` (a sensible in-stack far limit)

Because the helper derives `stack_guard_limit` from its own frame, it is best
called from a program top-level function (or equivalent stable entry frame)
that defines the intended stack region for switched tealets.

**Size behavior:**
- if `stack_integrity_bytes != 0`: uses caller value
- if `stack_integrity_bytes == 0`: uses one Linux page when available, otherwise `4096`

This helper is intended as a simple one-way "enable checks" API; use `tealet_configure_set()` for custom profiles.

---

### `tealet_config_set_locking()`

```c
int tealet_config_set_locking(tealet_t *tealet, const tealet_lock_t *locking);
```

Configure optional lock/unlock callbacks for a main-tealet domain.

`tealet_lock_t` contains:
- `tealet_lock_mode_t mode`
- `void (*lock)(void *arg)`
- `void (*unlock)(void *arg)`
- `void *arg`

The descriptor is copied into internal main-tealet state. Pass `NULL` to clear it.

**Recommendation:** Call this immediately after `tealet_initialize()` and before sharing tealet handles across threads. This avoids races where foreign-thread delete operations run before lock callbacks are installed.

Mode behavior:

- `TEALET_LOCK_OFF`: no internal auto-locking.
- `TEALET_LOCK_SWITCH`: libtealet automatically acquires/releases this lock for the core switching APIs:
- `tealet_new()`
- `tealet_create()`
- `tealet_switch()`
- `tealet_exit()`
- `tealet_fork()`

Rationale: these paths are temporally asymmetric (execution may leave one call site and later continue from a different logical point, including entry/exit boundaries). Keeping lock ownership inside the library for these transitions avoids brittle cross-frame lock management in integrator code.

Stub helpers in `tealet_extras.h` (`tealet_stub_new()`, `tealet_stub_run()`) are switching wrappers by inference, so they inherit this behavior through the core switching APIs.

For other APIs, integrators should call `tealet_lock()`/`tealet_unlock()` explicitly when foreign-thread access is possible.

Automatic locking in `TEALET_LOCK_SWITCH` mode does not require recursive lock primitives. Integrators may intentionally assert non-recursion in callbacks to enforce correct API usage.

**Allocator interaction contract:** libtealet may invoke allocator callbacks either with or without the configured tealet lock held, depending on internal call path. Integrators must not rely on the tealet lock as allocator protection, and allocator implementations must avoid deadlocking in either state.

---

### `tealet_lock()` / `tealet_unlock()`

```c
void tealet_lock(tealet_t *tealet);
void tealet_unlock(tealet_t *tealet);
```

Invoke the configured locking callbacks for the tealet domain.

**Behavior:**
- If lock callbacks are configured, these forward to them with the configured `arg`.
- If callbacks are not configured, both APIs are no-ops.

These helpers are intended for synchronizing access to tealet structures for the primary multi-threaded use case: foreign-thread `tealet_delete()` of non-main tealets.

Only the five core switching APIs are auto-locked by libtealet (`tealet_new()`, `tealet_create()`, `tealet_switch()`, `tealet_exit()`, `tealet_fork()`). For other APIs (for example `tealet_delete()` and `tealet_duplicate()`), integrators are expected to apply explicit lock/unlock around calls if those operations may run from foreign threads.

Switching itself remains thread-affine: switching between related tealets from different threads is unsupported, and cross-thread `tealet_switch()` usage is invalid.

Use a real cross-thread mutex (or equivalent) for these callbacks if you share handles across threads. No-op or thread-local locks do not provide inter-thread safety.

---

### `tealet_delete()`

```c
void tealet_delete(tealet_t *t);
```

Explicitly delete a tealet and free its resources.

**Parameters:**
- `t`: Tealet to delete (must not be currently executing)

**Usage:**
```c
tealet_t *t = tealet_create(main, my_run, NULL);
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

Allocator callbacks may be invoked either with or without the configured tealet lock held. Design allocators to be safe in both contexts and do not assume lock state.

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

## Helper Extensions (`tealet_extras.h`)

These APIs are convenience helpers built on top of the core tealet primitives (`tealet_create`, `tealet_switch`, `tealet_malloc`, and `tealet_duplicate`).

For backward compatibility, `tools.h` remains as an alias header that includes `tealet_extras.h`.

They are optional extensions intended for common patterns (stats-collecting allocator and copyable stubs). For low-level behavior details, you can inspect the implementation in `src/tealet_extras.c`.

### `tealet_statsalloc_init()`

```c
void tealet_statsalloc_init(tealet_statsalloc_t *alloc, tealet_alloc_t *base);
```

Initialize a wrapper allocator that tracks active allocation count and total bytes.

**Parameters:**
- `alloc`: Stats allocator state to initialize
- `base`: Underlying allocator used for actual memory operations

**Notes:**
- `alloc->alloc` becomes a valid `tealet_alloc_t` for `tealet_initialize()`.
- Counters are maintained in `n_allocs` and `s_allocs`.

### `tealet_stub_new()`

```c
tealet_t *tealet_stub_new(tealet_t *tealet, void *stack_far);
```

Create a paused stub tealet that can later run arbitrary functions via `tealet_stub_run()`.

**Parameters:**
- `tealet`: Main/owner tealet context
- `stack_far`: Optional far-boundary requirement for the initial stub stack snapshot (`NULL` uses default). This behaves like `tealet_create()`/`tealet_new()`: it can only extend capture range, never shrink it.

**Returns:** New stub tealet, or `NULL` on allocation failure.

### `tealet_stub_run()`

```c
int tealet_stub_run(tealet_t *stub, tealet_run_t run, void **parg);
```

Run a previously created stub as if it were a freshly created tealet.

**Parameters:**
- `stub`: Result of `tealet_stub_new()` (or a duplicate of such a stub)
- `run`: Run function to execute
- `parg`: Optional argument pointer for in/out switch data

**Returns:**
- `0` on success
- Negative error code on failure (`TEALET_ERR_MEM`, etc.)

**Important:** Behavior is undefined if `stub` was not created from the stub mechanism.

---

## Error handling

Error handling in libtealet is intentionally narrow:

- **Runtime/resource errors out of user control:**
    - `TEALET_ERR_MEM`
    - `TEALET_ERR_DEFUNCT`
    - `TEALET_ERR_PANIC` (main-only switch outcome)
- **Other negative returns** are generally caused by incorrect API usage (invalid context, invalid switching flow, integrity/caller sanity violations) and should be treated as effectively fatal programming errors.

### Practical handling guide

For `tealet_switch()`:
- On `TEALET_ERR_MEM`: treat as transient resource failure; free optional resources and either retry later or abort the coroutine workflow cleanly.
- On `TEALET_ERR_DEFUNCT`: treat target as unusable, inspect status, and delete/replace that tealet.
- On `TEALET_ERR_PANIC` (main only): treat as emergency reroute signal from `tealet_exit()`; report and terminate cleanly.

For `tealet_new()`:
- The same underlying switch failure conditions apply (`TEALET_ERR_MEM`, `TEALET_ERR_DEFUNCT`, `TEALET_ERR_PANIC`), but the API surface is pointer-based, so these failures are observed as `NULL`.

Practical note for `tealet_switch()` failures (`TEALET_ERR_MEM`, `TEALET_ERR_DEFUNCT`, `TEALET_ERR_PANIC`):
- In non-main tealets, the usual recovery path is to perform local cleanup and then exit to `main`.
- In the main tealet, the usual recovery path is to report the error and terminate the thread/process cleanly.

For `tealet_exit()`:
- In normal (non-`TEALET_EXIT_DEFER`) use, `tealet_exit()` is the panic path: it first attempts the requested target and then attempts fallback to `main`.
- To preserve forward progress during low-memory exit paths, the implementation may accept degraded state and mark affected saved stacks/tealets as defunct.
- If the requested target is defunct, exit is rerouted to `main`.
- The main tealet itself is never marked defunct.

### Note

`TEALET_ERR_PANIC` is currently the dedicated signal for reroute-to-main panic conditions after `tealet_exit()` target failure.

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
tealet_t *t = tealet_create(main, my_run, NULL);
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

Target tealet is defunct (invalid for switching).

**Causes:**
- Saved stack became invalid (for example due to a non-recoverable save/grow failure path)
- Tealet was left in an unusable state by an internal failure path
- Caller attempted to switch to a non-switchable tealet handle

**Recovery:**
- Check `tealet_status()` before switching
- Delete defunct tealets
- Don't reuse defunct tealet handles
- Note that the main tealet is never defunct

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

### `TEALET_ERR_PANIC`

```c
#define TEALET_ERR_PANIC (-6)
```

Main-tealet switch result signaling panic reroute.

**When returned:**
- Returned by `tealet_switch()` in `main` when a non-main tealet called `tealet_exit()` to a defunct target and libtealet rerouted the exit to `main`.

**Recovery:**
- Treat as fatal control-flow anomaly for the coroutine workflow.
- Log/report context and terminate the thread/process cleanly.

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
tealet_t *t1 = tealet_create(main, func1, NULL);
tealet_t *t2 = tealet_create(main, func2, NULL);
setup_relationship(t1, t2);  /* Both exist but not started */
void *arg = t2;
tealet_switch(t1, &arg);  /* Now start t1 */

/* Pattern 2: Create and start (more concise) */
void *arg = my_data;
tealet_t *t = tealet_new(main, my_func, &arg, NULL);
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

tealet_t *stub = tealet_create(main, stub_run, NULL);
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
    tealet_t *counter = tealet_create(main, counter_run, NULL);
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
