# API Reference

Complete reference for the libtealet API. Core functions are declared in `tealet.h`; helper extensions are declared in `tealet_extras.h`.

## Lifecycle Management

### tealet_initialize()

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

### tealet_finalize()

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

## Coroutine Creation

### tealet_new()

```c
tealet_t *tealet_new(tealet_t *tealet);
```

Allocate a new unbound tealet.

**Parameters:**
- `tealet`: Any tealet from the same main lineage (commonly `main`)

**Returns:**
- non-`NULL` on success
- `NULL` on allocation failure

**Usage:**
```c
tealet_t *t = tealet_new(main);
```

`tealet_new()` does not bind a run function and does not switch. Use `tealet_run()` to bind behavior.

---

### tealet_run()

```c
int tealet_run(tealet_t *tealet, tealet_run_t run, void **parg, void *stack_far, int flags);
```

Bind an unbound tealet to a run function and initialize its initial saved stack snapshot.

**Parameters:**
- `tealet`: NEW/unbound target tealet (typically from `tealet_new()`)
- `run`: Function to execute in the tealet context
- `parg`: Optional in/out argument pointer used with `TEALET_RUN_SWITCH`
- `stack_far`: Optional far-boundary requirement for the initial stack snapshot (`NULL` uses default)
- `flags`: `TEALET_RUN_DEFAULT` or `TEALET_RUN_SWITCH`

**Returns:**
- `0` on success
- negative `TEALET_ERR_*` on failure

`TEALET_RUN_DEFAULT` captures initial state and returns without switching. Start later with `tealet_switch()`.

`TEALET_RUN_SWITCH` captures state and immediately switches to the target tealet.
Conceptually, this is equivalent to `TEALET_RUN_DEFAULT` followed by `tealet_switch()`, but implemented as a single optimized path that avoids redundant internal state transitions.

**Usage (deferred start):**
```c
tealet_t *t = tealet_new(main);
tealet_run(t, my_run, NULL, NULL, TEALET_RUN_DEFAULT);

void *arg = my_data;
tealet_switch(t, &arg, TEALET_SWITCH_DEFAULT);
```

**Usage (create and start immediately):**
```c
void *arg = my_data;
tealet_t *t = tealet_new(main);
tealet_run(t, my_run, &arg, NULL, TEALET_RUN_SWITCH);
```

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

    worker = tealet_new(main);
    tealet_run(worker, my_run, &arg, stack_far, TEALET_RUN_SWITCH);
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
- `arg`: Argument passed via `tealet_switch()` or `tealet_run()`

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
    tealet_switch(current->main, &data, TEALET_SWITCH_DEFAULT);
    
    /* More work */
    
    /* Exit by returning */
    return current->main;
}
```

The run function executes until it returns or calls `tealet_exit()`. On return, execution transfers to the returned tealet and, by default, the current tealet remains allocated until explicitly deleted. However, a prior deferred `tealet_exit()` request with `TEALET_EXIT_DELETE` can still cause the tealet to be deleted when the run function returns.

---

### Advanced: Fork-like Semantics

⚠️ **Advanced Feature:** Fork-like semantics break the traditional function-scope discipline. Use with caution.

### tealet_set_far()

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

### tealet_fork()

```c
int tealet_fork(tealet_t *child, tealet_t **pother, void **parg, int flags);
```

Fork the current tealet into a NEW child tealet, duplicating execution state.

**Parameters:**
- `child`: NEW/unbound tealet (from `tealet_new()`) that receives forked child state
- `pother`: Pointer to receive the "other" tealet pointer:
  - In parent: receives pointer to child
  - In child: receives pointer to parent
- `parg`: Pointer to argument pointer for passing values to the suspended side. Can be NULL if no argument passing is desired (similar to `tealet_run()` and `tealet_switch()`)
    - With `TEALET_RUN_DEFAULT`: Parent continues, child is suspended. When parent switches to child, child receives value via `*parg`
    - With `TEALET_RUN_SWITCH`: Child continues, parent is suspended. When child switches back, parent receives value via `*parg`
    - See `tealet_run()` for detailed argument passing semantics
- `flags`: Fork mode flags:
    - `TEALET_RUN_DEFAULT` (0): Child created suspended, parent continues
    - `TEALET_RUN_SWITCH` (1): Immediately switch to child after creation

**Returns:**
- Parent: `1` (default mode) - you are the parent
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
    
    tealet_t *child = tealet_new(main);
    tealet_t *other = NULL;
    void *arg = NULL;
    int result = tealet_fork(child, &other, &arg, TEALET_RUN_DEFAULT);
    
    if (result == 0) {
        /* This is the CHILD */
        printf("Child: parent is %p, received arg=%p\n", other, arg);
        
        /* CRITICAL: Forked tealets MUST use tealet_exit() */
        void *return_value = (void*)0x1234;
        tealet_exit(other, return_value, TEALET_EXIT_NOFAIL);  /* Switch back to parent */
        
        /* Should not reach here */
        abort();
        
    } else if (result > 0) {
        /* This is the PARENT */
        printf("Parent: child is %p\n", other);
        
        /* Switch to child with an argument */
        arg = (void*)0x5678;
        tealet_switch(other, &arg, TEALET_SWITCH_DEFAULT);
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

Traditional tealet creation (`tealet_new()` + `tealet_run()`) maintains clean function-scope discipline: each tealet exists within a specific function's execution. `tealet_fork()` can break this discipline when used on main-lineage execution, but behaves like a direct continuation clone when used inside a regular function-scoped tealet.

This feature mirrors functionality from Stackless Python but was historically omitted from libtealet to keep the API simple and safe. Use it when you need advanced patterns like continuation capture or coroutine cloning.

**Flags:**
- `TEALET_RUN_DEFAULT`: Child suspended, parent continues (Unix fork-like)
- `TEALET_RUN_SWITCH`: Immediately become the child, parent suspended

---

## Context Switching

### tealet_switch()

```c
int tealet_switch(tealet_t *target, void **parg, int flags);
```

Suspend current tealet and resume target tealet.

**Parameters:**
- `target`: Tealet to switch to
- `parg`: Pointer to argument pointer (passed to target, updated with return value)
- `flags`: Switch control flags (`TEALET_SWITCH_DEFAULT`, `TEALET_SWITCH_FORCE`, `TEALET_SWITCH_PANIC`, `TEALET_SWITCH_NOFAIL`)

**Returns:** 
- `0` on success
- `TEALET_ERR_MEM` on memory allocation failure while saving/restoring state
- `TEALET_ERR_DEFUNCT` if target is defunct
- `TEALET_ERR_PANIC` when resumed due to an explicit panic-tagged transfer (`tealet_exit()` or `tealet_switch()` with panic)
- Negative error code on failure

`TEALET_SWITCH_FORCE` applies the same non-failable save behavior as
`TEALET_EXIT_FORCE`:
- without FORCE, save-time memory failures are returned (`TEALET_ERR_MEM`)
- with FORCE, save-time memory failures are ignored so the requested transfer can proceed
    by marking affected non-main saved stacks/tealets defunct.

`TEALET_SWITCH_FORCE` can still return `TEALET_ERR_MEM` when the only way to
continue would require main-stack growth that fails under memory pressure.
Main is never marked defunct, so this edge case remains a hard memory failure.

This means a successful forced switch can make other tealets become defunct as
the trade-off for forward progress under memory pressure.

`TEALET_SWITCH_NOFAIL` applies a robust retry/fallback policy: first try the
requested target with `TEALET_SWITCH_FORCE`, then panic+force fallback to main
for `TEALET_ERR_MEM` / `TEALET_ERR_DEFUNCT` failures.
See the detailed policy discussion and conceptual retry flow under
`tealet_exit()` / `TEALET_EXIT_NOFAIL` below; `TEALET_SWITCH_NOFAIL` follows
the same shape with switch flags.

When the requested target is `main`, `TEALET_SWITCH_NOFAIL` is guaranteed to
succeed (transfer starts), because main is never allowed to become defunct.

`TEALET_SWITCH_NOFAIL` can still return `TEALET_ERR_PANIC`, because panic is a
legitimate switch-back signal (not a transfer failure to retry). In typical
usage, panic-tagged fallbacks target main, so this check is usually needed on
the main tealet's switch return path.

**Usage:**
```c
void *arg = my_data;
int result = tealet_switch(target, &arg, TEALET_SWITCH_DEFAULT);
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
    tealet_switch(pong, &data, TEALET_SWITCH_DEFAULT);
    /* data now contains "pong" */
    
    return current->main;
}

tealet_t *pong(tealet_t *current, void *arg) {
    void *data = "pong";
    tealet_switch(arg, &data, TEALET_SWITCH_DEFAULT);  /* Switch back to ping */
    return current->main;
}
```

⚠️ **Stack Safety:** Never pass pointers to stack-allocated data in `parg`. The stack becomes invalid when switching. Allocate shared data on the heap.

**Exception:** If you intentionally expand the new tealet's initial saved stack via `stack_far` (as shown in the `tealet_new()` example above), stack-based structures in that expanded region can be valid for that tealet's execution.

---

### tealet_exit()

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
- `TEALET_EXIT_DELETE`: Auto-delete tealet on exit; outstanding pointers to the exiting tealet become invalid after transfer
- `TEALET_EXIT_DEFER`:  Store flags and argument, return to caller.  Will be used when
  tealet exits later.
- `TEALET_EXIT_FORCE`: Force the requested transfer despite save-time memory pressure by defuncting affected non-main stacks as needed
- `TEALET_EXIT_PANIC`: Tag the receiving tealet's resumed switch return as `TEALET_ERR_PANIC`
- `TEALET_EXIT_NOFAIL`: Apply automatic retries (`FORCE`, then `PANIC|FORCE` to main fallback)

**Usage:**
```c
tealet_t *my_run(tealet_t *current, void *arg) {
    /* Do work */
    
    /* Exit and delete this tealet */
    tealet_exit(current->main, &result, TEALET_EXIT_DELETE | TEALET_EXIT_NOFAIL);
    
    /* Never reached */
}
```

**With TEALET_EXIT_DEFER:**
```c
tealet_t *my_run(tealet_t *current, void *arg) {
    if (should_exit) {
        /* store exit target and flags */
        tealet_exit(current->main, &result, TEALET_EXIT_DEFER);
        /* Returns here, can do cleanup */
        cleanup_resources();
    }
    
    /* return value ignored.
     * exit value from earlier will be used.
     * Tealet is not auto-deleted unless TEALET_EXIT_DELETE is requested
     */
    return NULL;
}
```

This function does not return on successful transfer.

**Returns:**
- `0` only when `TEALET_EXIT_DEFER` is used (deferred exit setup succeeds)
- Negative error code on failure
    - `TEALET_ERR_MEM` when save/restore cannot complete and `TEALET_EXIT_FORCE` is not set
    - `TEALET_ERR_DEFUNCT` when the requested target is defunct
    - `TEALET_ERR_INVAL` for invalid target/state

`tealet_exit()` does not implicitly reroute to another target.

`TEALET_EXIT_FORCE` mirrors `TEALET_SWITCH_FORCE` behavior:
- without FORCE, save-time memory failures are returned (`TEALET_ERR_MEM`)
- with FORCE, save-time memory failures are ignored so the requested transfer can proceed
    by marking affected non-main saved stacks/tealets defunct.

`TEALET_EXIT_FORCE` can still return `TEALET_ERR_MEM` when the only way to
continue would require main-stack growth that fails under memory pressure.

`TEALET_EXIT_NOFAIL` applies the same robust fallback policy used by implicit
run-function return handling:
- try requested target with `TEALET_EXIT_FORCE`
- reroute to main with `TEALET_EXIT_PANIC | TEALET_EXIT_FORCE` only on
    `TEALET_ERR_MEM` / `TEALET_ERR_DEFUNCT`
- return other errors unchanged
Main is never marked defunct, so this edge case remains a hard memory failure.

When the requested target is `main`, `TEALET_EXIT_NOFAIL` is guaranteed to
succeed (transfer starts), because main is never allowed to become defunct.

This means a successful forced exit can make other tealets become defunct as
the trade-off for forward progress under memory pressure.

**Note:** Returning from the run function does not auto-delete by default; it
uses `TEALET_EXIT_DEFAULT` semantics unless `TEALET_EXIT_DELETE` was requested
explicitly (for example via deferred exit flags).
Use `tealet_exit()` when you need explicit control over deletion or want to
exit from nested calls within the run function.

**Robust retry policy used by `TEALET_EXIT_NOFAIL` (conceptual flow):**
```c
/* Conceptual helper: return non-robustness failures unchanged. */
static int exit_nofail_policy(tealet_t *self, tealet_t *target, void *arg, int base_flags) {
    int r;

    /* 1) First attempt: requested target, FORCE enabled. */
    r = tealet_exit(target, arg, base_flags | TEALET_EXIT_FORCE);

    if (r == TEALET_ERR_MEM || r == TEALET_ERR_DEFUNCT) {
        /* 2) Robustness failures: panic+force fallback to main. */
        (void)tealet_exit(self->main, arg, base_flags | TEALET_EXIT_PANIC | TEALET_EXIT_FORCE);
        abort();
    }

    /* 3) Other failures are returned unchanged by NOFAIL. */
    return r;
}
```

In normal code, prefer the built-in helper policy directly:

```c
tealet_exit(target, arg, base_flags | TEALET_EXIT_NOFAIL);
/* Non-returning on success */
```

---

## Status and Inspection

Query APIs in this section are not internally synchronized.

If they are used from foreign threads, or concurrently with switching/lifecycle
operations, callers must provide external synchronization (for example
`tealet_lock()`/`tealet_unlock()` around the access sequence).

### tealet_previous()

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

### tealet_status()

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
    tealet_switch(gen, &value, TEALET_SWITCH_DEFAULT);  /* Get next value */
}
```

---

### TEALET_IS_MAIN()

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

### TEALET_MAIN()

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

### tealet_get_origin()

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

### tealet_main_userpointer()

```c
void **tealet_main_userpointer(tealet_t *tealet);
```

Get the address of the per-main user pointer slot.

---

### TEALET_EXTRA()

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

### tealet_stack_diff()

```c
ptrdiff_t tealet_stack_diff(void *a, void *b);
```

Compute stack-relative distance between two addresses in a direction-aware way.

**Returns:** Positive when `b` is deeper on stack than `a`, zero if equal, negative otherwise.

---

### tealet_stack_further()

```c
void *tealet_stack_further(void *a, void *b);
```

Return whichever stack position is farther from the active stack top.

This is direction-aware:
- descending stacks: returns the greater address
- ascending stacks: returns the smaller address

Use this to combine boundary requirements from different stack references.

---

### tealet_new_probe()

```c
void *tealet_new_probe(tealet_t *dummy1, tealet_run_t dummy2, void **dummy3,
                       void *dummy4, int dummy5);
```

A function to test what stack boundary would be used for a new tealet without creating one. This is mostly informative.

**Parameters:**
- `dummy1`, `dummy2`, `dummy3`, `dummy4`, `dummy5`: Unused placeholders to match the `tealet_run()` call shape
- `dummy4`: Optional far-boundary requirement; if provided, it is clamped so it can only extend (not shrink) the probe boundary

**Returns:** The probed stack-boundary marker.

**Usage:**
```c
void *arg = NULL;
void *probe = tealet_new_probe(main, my_run, &arg, NULL, NULL);
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

### tealet_configure_get()

```c
int tealet_configure_get(tealet_t *tealet, tealet_config_t *config);
```

Read current effective runtime configuration from a main tealet.

**Notes:**
- Uses `tealet_config_t` size/version semantics.
- Writes only the caller-provided struct prefix (`config->size`).
- Returned values are canonicalized effective values.

---

### tealet_configure_set()

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

### tealet_configure_check_stack()

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

### tealet_config_set_locking()

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
- `TEALET_LOCK_SWITCH`: libtealet automatically acquires/releases this lock for the core switching APIs (`tealet_run()`, `tealet_switch()`, `tealet_exit()`, `tealet_fork()`).

Rationale: these paths are temporally asymmetric (execution may leave one call site and later continue from a different logical point, including entry/exit boundaries). Keeping lock ownership inside the library for these transitions avoids brittle cross-frame lock management in integrator code.

Stub helpers in `tealet_extras.h` (`tealet_stub_new()`, `tealet_stub_run()`) are switching wrappers by inference, so they inherit this behavior through the core switching APIs.

For other APIs, integrators should call `tealet_lock()`/`tealet_unlock()` explicitly when foreign-thread access is possible.

Automatic locking in `TEALET_LOCK_SWITCH` mode does not require recursive lock primitives. Integrators may intentionally assert non-recursion in callbacks to enforce correct API usage.

**Allocator interaction contract:** libtealet may invoke allocator callbacks either with or without the configured tealet lock held, depending on internal call path. Integrators must not rely on the tealet lock as allocator protection, and allocator implementations must avoid deadlocking in either state.

---

### tealet_lock() / tealet_unlock()

```c
void tealet_lock(tealet_t *tealet);
void tealet_unlock(tealet_t *tealet);
```

Invoke the configured locking callbacks for the tealet domain.

**Behavior:**
- If lock callbacks are configured, these forward to them with the configured `arg`.
- If callbacks are not configured, both APIs are no-ops.

These helpers are intended for synchronizing access to tealet structures for the primary multi-threaded use case: foreign-thread `tealet_delete()` of non-main tealets.

Only the core switching APIs are auto-locked by libtealet (`tealet_run()`, `tealet_switch()`, `tealet_exit()`, `tealet_fork()`). For other APIs (for example `tealet_delete()` and `tealet_duplicate()`), integrators are expected to apply explicit lock/unlock around calls if those operations may run from foreign threads.

Switching itself remains thread-affine: switching between related tealets from different threads is unsupported, and cross-thread `tealet_switch()` usage is invalid.

Use a real cross-thread mutex (or equivalent) for these callbacks if you share handles across threads. No-op or thread-local locks do not provide inter-thread safety.

---

### tealet_delete()

```c
void tealet_delete(tealet_t *t);
```

Explicitly delete a tealet and free its resources.

**Parameters:**
- `t`: Tealet to delete (must not be currently executing)

**Usage:**
```c
tealet_t *t = NULL;
t = tealet_new(main);
if (tealet_run(t, my_run, NULL, NULL, TEALET_RUN_DEFAULT) != 0) {
    /* Handle create failure */
}
/* Use t... */
tealet_delete(t);
```

**Important:**
- Cannot delete the currently executing tealet (use `tealet_exit()` instead)
- Cannot delete the main tealet (use `tealet_finalize()`)
- Tealets remain allocated when their run function returns
- Only call on tealets that are still allocated

**When to Use:**
- Deleting tealets that never ran (`TEALET_STATUS_INITIAL`)
- Cleaning up tealets kept alive with `TEALET_EXIT_DEFAULT`
- Cleaning up tealets that returned normally
- Manual resource management

**When Not Needed:**
- `tealet_exit()` with `TEALET_EXIT_DELETE` → automatic deletion

---

## Custom Allocators

### tealet_alloc_t

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

### TEALET_ALLOC_INIT_MALLOC

```c
#define TEALET_ALLOC_INIT_MALLOC { NULL, tealet_malloc_malloc, tealet_free_free }
```

Initializer for standard malloc/free allocator.

**Usage:**
```c
tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
```

---

## Helper Extensions (tealet_extras.h)

These APIs are convenience helpers built on top of the core tealet primitives (`tealet_new`, `tealet_run`, `tealet_switch`, `tealet_malloc`, and `tealet_duplicate`).

They are optional extensions intended for common patterns (stats-collecting allocator and copyable stubs). For low-level behavior details, you can inspect the implementation in `src/tealet_extras.c`.

### tealet_statsalloc_init()

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

### tealet_stub_new()

```c
int tealet_stub_new(tealet_t *tealet, tealet_t **pstub, void *stack_far);
```

Create a paused stub tealet that can later run arbitrary functions via `tealet_stub_run()`.

**Parameters:**
- `tealet`: Main/owner tealet context
- `pstub`: Output pointer receiving the created stub tealet on success
- `stack_far`: Optional far-boundary requirement for the initial stub stack snapshot (`NULL` uses default). This behaves like `tealet_run()`: it can only extend capture range, never shrink it.

**Returns:**
- `0` on success
- Negative `TEALET_ERR_*` on failure

### tealet_stub_run()

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
    switching APIs (`tealet_run()`, `tealet_switch()`, `tealet_exit()`,
    `tealet_fork()`).
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

## Error handling

Error handling in libtealet is intentionally narrow:

- **Runtime/resource errors out of user control:**
    - `TEALET_ERR_MEM`
    - `TEALET_ERR_DEFUNCT`
    - `TEALET_ERR_PANIC` (explicit panic-tagged resume)
- **Other negative returns** are generally caused by incorrect API usage (invalid context, invalid switching flow, integrity/caller sanity violations) and should be treated as effectively fatal programming errors.

### Practical handling guide

For `tealet_switch()`:
- On `TEALET_ERR_MEM`: treat as transient resource failure; free optional resources and either retry later or abort the coroutine workflow cleanly.
- On `TEALET_ERR_DEFUNCT`: treat target as unusable, inspect status, and delete/replace that tealet.
- On `TEALET_ERR_PANIC`: treat as explicit panic resume signal from
    `tealet_exit(..., TEALET_EXIT_PANIC)` or `tealet_switch(..., TEALET_SWITCH_PANIC)`.

Forced transfer note (`tealet_switch(..., TEALET_SWITCH_FORCE)` and
`tealet_exit(..., TEALET_EXIT_FORCE)`):
- these APIs can succeed while ignoring save-time memory errors
- in that success path, affected non-main saved stacks/tealets may be marked defunct.

For `tealet_run()`:
- `TEALET_ERR_MEM` is the bind/start failure case.
- `TEALET_ERR_PANIC` signals an unexpected panic-tagged switch-back during `TEALET_RUN_SWITCH` startup.

Practical note for switch/exit failures:
- In non-main tealets, the usual recovery path is to perform local cleanup and then exit to `main`.
- In the main tealet, the usual recovery path is to report the error and terminate the thread/process cleanly.

For `tealet_exit()` specifically:
- The requested target is never implicitly replaced by `main`; panic reroute must be explicit.
- The main tealet itself is never marked defunct.

### Note

`TEALET_ERR_PANIC` is the explicit panic-resume signal produced by
`tealet_exit(..., TEALET_EXIT_PANIC)` or `tealet_switch(..., TEALET_SWITCH_PANIC)`.

---

## Error Codes

### TEALET_ERR_MEM

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
tealet_t *t = tealet_new(main);
if (tealet_run(t, my_run, NULL, NULL, TEALET_RUN_DEFAULT) != 0) {
    fprintf(stderr, "Out of memory\n");
    return TEALET_ERR_MEM;
}
```

---

### TEALET_ERR_DEFUNCT

```c
#define TEALET_ERR_DEFUNCT (-2)
```

Target tealet is defunct (invalid for switching).

**Causes:**
- Successful forced transfer under memory pressure (`TEALET_SWITCH_FORCE` or
    `TEALET_EXIT_FORCE`) can ignore save-time memory errors and mark affected
    non-main saved stacks/tealets defunct.
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
int result = tealet_switch(t, &arg, TEALET_SWITCH_DEFAULT);
if (result == TEALET_ERR_DEFUNCT) {
    fprintf(stderr, "Tealet has exited\n");
    tealet_delete(t);
    return -1;
}
```

---

### TEALET_ERR_PANIC

```c
#define TEALET_ERR_PANIC (-6)
```

Switch result signaling explicit panic-tagged resume.

**When returned:**
- Returned by `tealet_switch()` when the resumed tealet was targeted via
    `tealet_exit(..., TEALET_EXIT_PANIC)` or `tealet_switch(..., TEALET_SWITCH_PANIC)`.

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
#define TEALET_EXIT_DELETE  1  /* Auto-delete on exit; pointers to exiting tealet become invalid */
#define TEALET_EXIT_DEFER   2  /* Defer exit to return */
#define TEALET_EXIT_FORCE   4  /* Force exit despite save-time memory failures */
#define TEALET_EXIT_PANIC   8  /* Mark receiving tealet as panic-resumed */
#define TEALET_EXIT_NOFAIL 16  /* Retry with FORCE, then panic+force to main */

/* Switch flags */
#define TEALET_SWITCH_DEFAULT 0  /* Default switch behavior */
#define TEALET_SWITCH_FORCE   4  /* Force switch despite save-time memory failures */
#define TEALET_SWITCH_PANIC   8  /* Mark receiving tealet as panic-resumed */
#define TEALET_SWITCH_NOFAIL 16  /* Retry with FORCE, then panic+force to main */
```

Used with `tealet_exit()`.

---

## Decision Trees

### When to Use Deferred vs Immediate Start

**Use `tealet_new()` + `tealet_run(..., TEALET_RUN_DEFAULT)`:**
- Setting up multiple coroutines before starting any
- Need to pass tealet pointer to other setup code
- Separating creation from execution for clarity

**Use `tealet_new()` + `tealet_run(..., TEALET_RUN_SWITCH)`:**
- Start execution immediately
- Don't need the tealet pointer before it runs
- More concise code

**Example Comparison:**
```c
/* Pattern 1: Deferred start (more control) */
tealet_t *t1 = tealet_new(main);
tealet_t *t2 = tealet_new(main);
tealet_run(t1, func1, NULL, NULL, TEALET_RUN_DEFAULT);
tealet_run(t2, func2, NULL, NULL, TEALET_RUN_DEFAULT);
setup_relationship(t1, t2);  /* Both exist but not started */
void *arg = t2;
tealet_switch(t1, &arg, TEALET_SWITCH_DEFAULT);  /* Now start t1 */

/* Pattern 2: Immediate start (more concise) */
void *arg = my_data;
tealet_t *t = tealet_new(main);
tealet_run(t, my_func, &arg, NULL, TEALET_RUN_SWITCH);
/* Already started, arg contains first return value */
```

---

### When to Use tealet_exit() vs Returning

**Return from run function:**
- Normal completion
- Simplest code path
- Keeps tealet allocated by default; delete it later with `tealet_delete()`
- Can only specify exit target tealet, no flags.

**Note:** A prior deferred exit request with `TEALET_EXIT_DELETE` can still
cause deletion when the run function eventually returns.

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
        tealet_exit(current->main, &error, TEALET_EXIT_DELETE | TEALET_EXIT_NOFAIL);
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

Use stubs when you want to pre-capture a stack position once, then launch one or
many tealets from that same baseline.

The helper API is in `tealet_extras.h`:
- `tealet_stub_new()` creates a paused trampoline tealet.
- `tealet_duplicate()` can clone that paused stub cheaply.
- `tealet_stub_run()` starts a specific run function on the stub or clone.

This is useful for:
- launching a family of similar tealets from the same captured stack depth,
- reducing repeated setup work for create-and-start flows,
- generating reproducible starting contexts in tests.

Example:

```c
tealet_t *worker_run(tealet_t *current, void *arg) {
    /* Worker body */
    return current->main;
}

tealet_t *stub = NULL;
if (tealet_stub_new(main, &stub, NULL) != 0) {
    /* allocation/setup failure */
}

tealet_t *clone = tealet_duplicate(stub);
if (clone == NULL) {
    tealet_delete(stub);
    /* allocation failure */
}

void *arg = NULL;
tealet_stub_run(clone, worker_run, &arg);

tealet_delete(clone);
tealet_delete(stub);
```

Repository references:
- `tests/tests.c` uses stub flows in `stub_new()`, `stub_new2()`, `stub_new3()`.
- `src/tealet_extras.c` contains the trampoline implementation (`_tealet_stub_main`).

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
        tealet_switch(current->main, &value, TEALET_SWITCH_DEFAULT);
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
    tealet_t *counter = tealet_new(main);
    if (tealet_run(counter, counter_run, NULL, NULL, TEALET_RUN_DEFAULT) != 0) {
        fprintf(stderr, "Failed to create counter\n");
        tealet_finalize(main);
        return 1;
    }
    
    /* Start it */
    int max = 5;
    void *arg = &max;
    int result = tealet_switch(counter, &arg, TEALET_SWITCH_DEFAULT);
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
        
        result = tealet_switch(counter, &arg, TEALET_SWITCH_DEFAULT);
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
