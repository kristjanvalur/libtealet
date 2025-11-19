# Getting Started with libtealet

**libtealet** enables symmetric coroutines (co-stacks) in C without requiring compiler support. Unlike async/await patterns, libtealet can suspend and resume entire call stacks at any point.

## Installation

### Using Pre-built Libraries

Download the latest source distribution from the [releases page](https://github.com/kristjanvalur/libtealet/releases) or clone the repository:

```bash
git clone --recursive https://github.com/kristjanvalur/libtealet.git
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
    tealet_t *coro = tealet_new(main, hello_run, &arg);
    
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
    tealet_switch(current->main, &ptr);
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
    
    tealet_switch(current->main, &ptr);
    /* ptr still valid */
    
    free(heap_value);
    return current->main;
}
```

**Rule of thumb:** If data needs to survive a `tealet_switch()`, allocate it on the heap.

## Tealet Lifecycle and Exiting

### Best Practice: Always Use `tealet_exit()`

While a tealet run function can simply `return` to exit, **it's strongly recommended to use `tealet_exit()` explicitly**:

```c
tealet_t *my_run(tealet_t *current, void *arg) {
    printf("Doing work...\n");
    
    /* ✅ Recommended: Explicit exit */
    tealet_exit(current->main, NULL, TEALET_FLAG_DELETE);
    
    /* Should not reach here */
    return current->main;  /* Fallback only */
}
```

**Why use `tealet_exit()`?**
- More explicit about where execution goes
- Controls auto-deletion with flags
- Required for forked tealets (see Advanced section)
- Clearer intent in complex switching scenarios

### ⚠️ Auto-Delete Danger

When a run function returns (or calls `tealet_exit()` with `TEALET_FLAG_DELETE`), the tealet is **automatically deleted**. This can cause dangling pointer issues:

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
    tealet_t *worker = tealet_new(main, worker_run, &arg);
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
    tealet_exit(current->main, NULL, 0);  /* or TEALET_FLAG_NONE */
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    void *arg = NULL;
    tealet_t *worker = tealet_new(main, worker_run, &arg);
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

- `TEALET_FLAG_DELETE` or `TEALET_FLAG_NONE` (0): Auto-delete on exit (default behavior when returning)
- `0` (no flags): Don't auto-delete, manual `tealet_delete()` required
- `TEALET_FLAG_DEFER`: For use with run function returns (advanced, see API docs)

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
        tealet_switch(current->main, NULL);  /* Yield back */
    }
    tealet_exit(current->main, NULL, 0);  /* Don't auto-delete */
    return current->main;
}

int main(void) {
    /* ... */
    tealet_t *worker = tealet_new(main, controlled_worker, &arg);
    
    /* Can switch back to worker multiple times */
    tealet_switch(worker, NULL);
    tealet_switch(worker, NULL);
    
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
        tealet_switch(pong, NULL);
    }
    
    return current->main;
}

tealet_t *pong_run(tealet_t *current, void *arg) {
    tealet_t *ping = (tealet_t*)arg;
    
    for (int i = 0; i < 3; i++) {
        printf("  Pong %d\n", i);
        tealet_switch(ping, NULL);
    }
    
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* Create both coroutines */
    tealet_t *ping = tealet_create(main, ping_run);
    tealet_t *pong = tealet_create(main, pong_run);
    
    /* Start ping, passing pong as argument */
    void *arg = pong;
    tealet_switch(ping, &arg);
    
    /* Start pong, passing ping as argument */
    arg = ping;
    tealet_switch(pong, &arg);
    
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
        tealet_switch(current->main, &value);
    }
    
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* Create generator for range(5) */
    int max = 5;
    void *arg = &max;
    tealet_t *gen = tealet_new(main, range_run, &arg);
    
    /* Pull values from generator */
    while (tealet_status(gen) == TEALET_STATUS_ACTIVE) {
        printf("%d\n", (int)(intptr_t)arg);
        tealet_switch(gen, &arg);
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
        tealet_switch(ctx->consumer, NULL);
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
        tealet_switch(producer, NULL);
    }
    
    return current->main;
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    /* Shared buffer on heap */
    int *buffer = malloc(sizeof(int));
    
    /* Create consumer first */
    tealet_t *consumer = tealet_create(main, consumer_run);
    
    /* Create producer context */
    producer_ctx_t ctx = {consumer, buffer, 0};
    void *arg = &ctx;
    tealet_t *producer = tealet_new(main, producer_run, &arg);
    
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
tealet_t *t = tealet_new(main, my_run, &arg);
/* Returns when my_run switches back */
/* arg now contains return value */
```

Use when you want to start execution immediately.

### `tealet_create()` + `tealet_switch()` - Deferred Start

```c
tealet_t *t = tealet_create(main, my_run);
/* Tealet created but not yet running */

void *arg = my_data;
tealet_switch(t, &arg);  /* Now it starts */
```

Use when you need to set up multiple coroutines before starting any.

## Error Handling

Functions return `NULL` or negative error codes on failure:

```c
tealet_t *t = tealet_create(main, my_run);
if (t == NULL) {
    fprintf(stderr, "Failed to create tealet (out of memory)\n");
    return TEALET_ERR_MEM;
}

int result = tealet_switch(t, &arg);
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
tealet_t *t1 = tealet_create(main1, func1);

/* Thread 2 */
tealet_t *main2 = tealet_initialize(&alloc, 0);
tealet_t *t2 = tealet_create(main2, func2);

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
