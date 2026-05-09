# Getting Started with libtealet

**libtealet** enables symmetric coroutines (co-stacks) in C without requiring compiler support. Unlike async/await patterns, libtealet can suspend and resume entire call stacks at any point.

## Installation

### Using Pre-built Libraries

Download the latest source distribution from the [releases page](https://github.com/kristjanvalur/libtealet/releases) or clone the repository:

```bash
git clone https://github.com/kristjanvalur/libtealet.git
cd libtealet
make
```

The library will be built in `bin/`:
- `bin/libtealet.so` - Shared library
- `bin/libtealet.a` - Static library

### Running Tests

```bash
make test
```

### Formatting checks (development)

`clang-format` is required for local contributor formatting checks and for CI parity.

Install `clang-format`:

- Linux (Debian/Ubuntu): `sudo apt install clang-format`
- macOS (Homebrew): `brew install clang-format`
- Windows:
    - `winget install LLVM.LLVM`
    - or `choco install llvm`

Then run:

```bash
make check-format   # verify formatting
make format         # apply formatting
```

### API docs generation (development)

`doxygen` is used to generate local API documentation from header comments and Markdown guides.

Install tools:

- Linux (Debian/Ubuntu): `sudo apt install doxygen graphviz`
- macOS (Homebrew): `brew install doxygen graphviz`
- Windows:
    - `winget install DimitriVanHeesch.Doxygen`
    - `winget install Graphviz.Graphviz`
    - or `choco install doxygen.install graphviz`

Then run:

```bash
make docs        # generate docs/_build/doxygen/html/index.html
make docs-check  # fail on Doxygen warnings
make docs-clean  # remove generated docs
```

See `docs/DOXYGEN.md` for the hybrid authored+generated documentation workflow.

## Hello Coroutine

The simplest example demonstrates creating and switching between coroutines:

```c
#include <stdio.h>
#include "tealet.h"

/* The run function executes in the tealet's context */
tealet_t *hello_run(tealet_t *current, void *arg) {
    printf("Hello from coroutine!\n");
    printf("Argument: %s\n", (char*)arg);
    
    /* Return to the main tealet */
    return current->main;
}

int main(void) {
    /* Initialize using standard malloc */
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* Create and run a coroutine */
    void *arg = "Hello World";
    tealet_t *coro = NULL;
    if (tealet_new(main, &coro, hello_run, &arg, NULL) != 0) {
        tealet_finalize(main);
        return 1;
    }
    
    /* Clean up */
    tealet_finalize(main);
    return 0;
}
```

Compile and run:
```bash
gcc -o hello hello.c -Isrc -Lbin -ltealet
./hello
```

Output:
```
Hello from coroutine!
Argument: Hello World
```

## Memory Safety Rules

⚠️ **Critical:** Stack-allocated data becomes invalid when switching contexts.

### ❌ Wrong - Stack Data

```c
tealet_t *my_run(tealet_t *current, void *arg) {
    int local_value = 42;  /* On the stack! */
    void *ptr = &local_value;
    
    /* DANGER: Switching invalidates local_value */
    tealet_switch(current->main, &ptr, TEALET_SWITCH_DEFAULT);
    /* ptr now points to invalid stack data */
    
    return current->main;
}
```

### ✅ Correct - Heap Data

```c
tealet_t *my_run(tealet_t *current, void *arg) {
    /* Allocate on heap - use malloc or your own allocator */
    int *heap_value = malloc(sizeof(int));
    *heap_value = 42;
    void *ptr = heap_value;
    
    tealet_switch(current->main, &ptr, TEALET_SWITCH_DEFAULT);
    /* ptr still valid */
    
    free(heap_value);
    return current->main;
}
```

**Rule of thumb:** If data needs to survive a `tealet_switch()`, allocate it on the heap.
**Exception:** You can intentionally include additional creator-frame stack data in a new tealet's initial snapshot by passing `stack_far` to `tealet_new()` or `tealet_create()`. This exists to make integration practical for existing code paths that already rely on stack-based structures.

## Tealet Lifecycle and Exiting

### Best Practice: Always Use `tealet_exit()`

While a tealet run function can simply `return` to exit, **it's strongly recommended to use `tealet_exit()` explicitly**:

```c
tealet_t *my_run(tealet_t *current, void *arg) {
    printf("Doing work...\n");
    
    /* ✅ Recommended: Explicit exit */
    tealet_exit(current->main, NULL, TEALET_EXIT_DELETE);
    
    /* Should not reach here */
    return current->main;  /* Fallback only */
}
```

**Why use `tealet_exit()`?**
- More explicit about where execution goes
- Controls auto-deletion with flags
- Required for main-lineage fork flows (see Advanced section)
- Clearer intent in complex switching scenarios

### ⚠️ Auto-Delete Danger

When a run function returns (or calls `tealet_exit()` with `TEALET_EXIT_DELETE`), the tealet is **automatically deleted**. This can cause dangling pointer issues:

```c
/* ❌ DANGER: Race condition */
tealet_t *worker_run(tealet_t *current, void *arg) {
    printf("Quick work\n");
    return current->main;  /* Auto-deletes this tealet */
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    void *arg = NULL;
    tealet_t *worker = NULL;
    /* Assume tealet_new() returns 0 in this illustration. */
    tealet_new(main, &worker, worker_run, &arg, NULL);
    /* worker may already be deleted here! */
    
    int status = tealet_status(worker);  /* ❌ Dangling pointer! */
    
    tealet_finalize(main);
    return 0;
}
```

**What happened?** `tealet_new()` creates the worker and **immediately runs it**. The worker completes and deletes itself **before** `tealet_new()` returns. Now `worker` is a dangling pointer.

### ✅ Safe Pattern: Prevent Auto-Delete

```c
tealet_t *worker_run(tealet_t *current, void *arg) {
    printf("Work done\n");
    /* Exit without auto-delete */
    tealet_exit(current->main, NULL, TEALET_EXIT_DEFAULT);
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    void *arg = NULL;
    tealet_t *worker = NULL;
    /* Assume tealet_new() returns 0 in this illustration. */
    tealet_new(main, &worker, worker_run, &arg, NULL);
    /* worker still exists and can be queried */
    
    int status = tealet_status(worker);  /* ✅ Safe */
    printf("Worker status: %d\n", status);
    
    /* Manual cleanup */
    tealet_delete(worker);
    tealet_finalize(main);
    return 0;
}
```

### Exit Flags

- `TEALET_EXIT_DEFAULT` (0): **Prevent auto-delete**; tealet must be manually deleted with `tealet_delete()`
- `TEALET_EXIT_DELETE`: **Auto-delete on exit**; same as returning from the run function (the default behavior)
- `TEALET_EXIT_DEFER`: **Defer deletion to run function return** (advanced; see API docs)

**Note:** The old `TEALET_FLAG_*` names are still available for backwards compatibility.

### Shutdown Order Requirement

At shutdown, always delete non-main tealets before calling `tealet_finalize(main)`.

- `tealet_finalize()` frees the main tealet and does not walk child tealets.
- After `tealet_finalize()`, all tealet handles from that main tealet are invalid.
- That includes `tealet_delete()` and `tealet_free()`, because allocator/context access goes through the main tealet.

There is no supported way to decouple this order.

### When to Use Each

**Auto-delete (default):**
```c
tealet_t *fire_and_forget(tealet_t *current, void *arg) {
    /* Do work that doesn't need caller intervention */
    printf("Task complete\n");
    return current->main;  /* Auto-deletes */
}
```

**Manual delete:**
```c
tealet_t *controlled_worker(tealet_t *current, void *arg) {
    while (should_continue()) {
        do_work();
        tealet_switch(current->main, NULL, TEALET_SWITCH_DEFAULT);  /* Yield back */
    }
    tealet_exit(current->main, NULL, TEALET_EXIT_DEFAULT);  /* Don't auto-delete */
    return current->main;
}

int main(void) {
    /* ... */
    tealet_t *worker = NULL;
    tealet_new(main, &worker, controlled_worker, &arg, NULL);
    
    /* Can switch back to worker multiple times */
    tealet_switch(worker, NULL, TEALET_SWITCH_DEFAULT);
    tealet_switch(worker, NULL, TEALET_SWITCH_DEFAULT);
    
    /* Manual cleanup when done */
    tealet_delete(worker);
    /* ... */
}
```

## Common Patterns

### Ping-Pong: Two Coroutines Alternating

```c
#include <stdio.h>
#include "tealet.h"

tealet_t *ping_run(tealet_t *current, void *arg) {
    tealet_t *pong = (tealet_t*)arg;
    
    for (int i = 0; i < 3; i++) {
        printf("Ping %d\n", i);
        tealet_switch(pong, NULL, TEALET_SWITCH_DEFAULT);
    }
    
    return current->main;
}

tealet_t *pong_run(tealet_t *current, void *arg) {
    tealet_t *ping = (tealet_t*)arg;
    
    for (int i = 0; i < 3; i++) {
        printf("  Pong %d\n", i);
        tealet_switch(ping, NULL, TEALET_SWITCH_DEFAULT);
    }
    
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* Create both coroutines */
    tealet_t *ping = NULL;
    tealet_t *pong = NULL;
    tealet_create(main, &ping, ping_run, NULL);
    tealet_create(main, &pong, pong_run, NULL);
    
    /* Start ping, passing pong as argument */
    void *arg = pong;
    tealet_switch(ping, &arg, TEALET_SWITCH_DEFAULT);
    
    /* Start pong, passing ping as argument */
    arg = ping;
    tealet_switch(pong, &arg, TEALET_SWITCH_DEFAULT);
    
    tealet_delete(ping);
    tealet_delete(pong);
    tealet_finalize(main);
    return 0;
}
```

Output:
```
Ping 0
  Pong 0
Ping 1
  Pong 1
Ping 2
  Pong 2
```

### Generator: Yielding Values

```c
#include <stdio.h>
#include "tealet.h"

tealet_t *range_run(tealet_t *current, void *arg) {
    int max = *(int*)arg;
    
    for (int i = 0; i < max; i++) {
        /* Yield value back to caller */
        void *value = (void*)(intptr_t)i;
        tealet_switch(current->main, &value, TEALET_SWITCH_DEFAULT);
    }
    
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* Create generator for range(5) */
    int max = 5;
    void *arg = &max;
    tealet_t *gen = NULL;
    tealet_new(main, &gen, range_run, &arg, NULL);
    
    /* Pull values from generator */
    while (tealet_status(gen) == TEALET_STATUS_ACTIVE) {
        printf("%d\n", (int)(intptr_t)arg);
        tealet_switch(gen, &arg, TEALET_SWITCH_DEFAULT);
    }
    
    tealet_delete(gen);
    tealet_finalize(main);
    return 0;
}
```

Output:
```
0
1
2
3
4
```

### Producer-Consumer

```c
#include <stdio.h>
#include "tealet.h"

typedef struct {
    tealet_t *consumer;
    int *buffer;
    int count;
} producer_ctx_t;

tealet_t *producer_run(tealet_t *current, void *arg) {
    producer_ctx_t *ctx = (producer_ctx_t*)arg;
    
    /* Produce 10 items */
    for (int i = 0; i < 10; i++) {
        *ctx->buffer = i * i;  /* Produce square numbers */
        printf("Produced: %d\n", *ctx->buffer);
        
        /* Switch to consumer */
        tealet_switch(ctx->consumer, NULL, TEALET_SWITCH_DEFAULT);
    }
    
    return current->main;
}

tealet_t *consumer_run(tealet_t *current, void *arg) {
    producer_ctx_t *ctx = (producer_ctx_t*)arg;
    tealet_t *producer = (tealet_t*)arg;  /* Passed on first switch */
    
    while (tealet_status(producer) == TEALET_STATUS_ACTIVE) {
        /* Consume item */
        printf("  Consumed: %d\n", *ctx->buffer);
        ctx->count++;
        
        /* Switch back to producer */
        tealet_switch(producer, NULL, TEALET_SWITCH_DEFAULT);
    }
    
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* Shared buffer on heap */
    int *buffer = malloc(sizeof(int));
    
    /* Create consumer first */
    tealet_t *consumer = NULL;
    tealet_create(main, &consumer, consumer_run, NULL);
    
    /* Create producer context */
    producer_ctx_t ctx = {consumer, buffer, 0};
    void *arg = &ctx;
    tealet_t *producer = NULL;
    tealet_new(main, &producer, producer_run, &arg, NULL);
    
    printf("Total consumed: %d items\n", ctx.count);
    
    free(buffer);
    tealet_delete(consumer);
    tealet_finalize(main);
    return 0;
}
```

## Creation Patterns

### `tealet_new()` - Create and Start Immediately

```c
void *arg = my_data;
tealet_t *t = NULL;
tealet_new(main, &t, my_run, &arg, NULL);
/* Returns when my_run switches back */
/* arg now contains return value */
```

Use when you want to start execution immediately.

### `tealet_create()` + `tealet_switch()` - Deferred Start

```c
tealet_t *t = NULL;
tealet_create(main, &t, my_run, NULL);
/* Tealet created but not yet running */

void *arg = my_data;
tealet_switch(t, &arg, TEALET_SWITCH_DEFAULT);  /* Now it starts */
```

Use when you need to set up multiple coroutines before starting any.

## Error Handling

Functions return negative error codes on failure (and write created tealets via out-parameters):

```c
tealet_t *t = NULL;
if (tealet_create(main, &t, my_run, NULL) != 0) {
    fprintf(stderr, "Failed to create tealet (out of memory)\n");
    return TEALET_ERR_MEM;
}

int result = tealet_switch(t, &arg, TEALET_SWITCH_DEFAULT);
if (result == TEALET_ERR_DEFUNCT) {
    fprintf(stderr, "Target tealet is corrupt\n");
    return -1;
}
```

Error codes:
- `TEALET_ERR_MEM` (-1): Memory allocation failed
- `TEALET_ERR_DEFUNCT` (-2): Target tealet is corrupt

Check status before switching:
```c
if (tealet_status(t) == TEALET_STATUS_DEFUNCT) {
    /* Tealet is corrupt, don't switch to it */
}
```

## Thread Safety

⚠️ Tealets are **not thread-safe** across threads.

**Rules:**
- Each thread must have its own main tealet
- Never switch between tealets from different threads
- Tealets can only switch if they share the same main tealet

```c
/* Thread 1 */
tealet_t *main1 = tealet_initialize(&alloc, 0);
tealet_t *t1 = NULL;
tealet_create(main1, &t1, func1, NULL);

/* Thread 2 */
tealet_t *main2 = tealet_initialize(&alloc, 0);
tealet_t *t2 = NULL;
tealet_create(main2, &t2, func2, NULL);

/* ❌ WRONG: Can't switch between t1 and t2 (different mains) */
/* ✅ OK: Can switch within same thread between t1 and other tealets from main1 */
```

## Next Steps

- Read [API.md](API.md) for complete function reference
- See [ARCHITECTURE.md](ARCHITECTURE.md) to understand internals
- Check `tests/setcontext.c` for a complete working example
- Explore `tests/tests.c` for advanced usage patterns

## Performance Characteristics

- **Context switch**: ~100-500 CPU cycles (platform-dependent)
- **Memory overhead**: Proportional to saved stack size
- **Stack saving**: Incremental (only saves needed portions)
- **No system calls**: Pure user-space operation

Compared to OS threads:
- ✅ Much faster context switching (no kernel involvement)
- ✅ Lower memory overhead per coroutine
- ❌ No true parallelism (runs on single OS thread)
- ❌ Manual scheduling required
