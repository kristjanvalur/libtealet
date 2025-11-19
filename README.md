[![build and test](https://github.com/kristjanvalur/libtealet/actions/workflows/build-test.yml/badge.svg)](https://github.com/kristjanvalur/libtealet/actions/workflows/build-test.yml)

# Libtealet

**Version 0.2.0**

LibTealet is a lightweight co-routine library for C.  It is based on the
technique of stack-slicing, where the execution stack is saved and restored
in order to maintain separate execution context.
It uses the [Stackman library][stackman] library for low
level stack operations, providing implementation for common modern desktop platforms.
There are no other run-time dependencies, ecxcept for `memcpy()`, and the default
use of `malloc()` can be replaced with a custom memory allocator, and `assert()` which is
used by debug builds.

## Coroutines
In today's common parlance, using _coroutines_ refers to co-operative multitasking within a single
operating system thread, where execution in a function stack is halted in order to be resumed
later.  This is commonly achieved using the [coroutine][coroutine] programming
construct, as available in Python 3.5 and later, and [C++20][cpp20]]
although these are technically _asymmetric coroutines_ since
they can only be suspended and resumed by their immediate caller.  To be able to suspend a stack
of functions, all of the functions involved must therefore be coroutines, often decorated with
a special keyword such as `async`.

Libtealet does not require special functions.  Its approach is more akin to threading
in that a whole execution stack is suspended, and control passed to another stack.
Perhaps it is prudent to talk of _co-stacks_ in this regard.  No special compiler or language
support is required.

## Stack-slicing
The approach used here employs *stack-slicing*, a term
coined by Christian Tismer to desctibe the technique employed by
Stackless-Python.  
Instead of each coroutine (or co-stack) having its own stack in virtual memory like an operating system thread does,
parts of the C stack that belong to different execution contexts are
stored on the heap and restored to the system stack as required.  

For each platform, a small piece of assembly code is required.
This code stores cpu registers on the stack, then calls functions to
save/restore the stack to or from the heap, and adjusts the stack pointer
as required.  Then the cpu state is restored from the restored stack
and a new co-routine is running.  This support is provided by the _Stackman_ library.

## Similar work
Stackless Python and Gevent use a similar mechanism, and this code is based on that work.

For C, there also exist some other approache. For an overview, see [Coroutines for C][coroc]

## Stack-chaining Optimization

Unlike traditional coroutine implementations that save entire stacks, libtealet uses an incremental stack-saving technique with chunk chaining. When switching contexts, only the portion of the stack that has changed is saved. Stack chunks are linked via a `g_prev` chain, enabling:

- **Incremental growth**: Small initial allocations (512 bytes), doubling on demand up to 256KB
- **Shared stack segments**: Coroutines that diverged from a common point share unchanged stack portions via reference counting
- **Memory efficiency**: Typical coroutine overhead is a few KB, not the 1-8MB of OS thread stacks

This makes libtealet suitable for applications with thousands of concurrent coroutines.

## Quick Start

```c
#include "tealet.h"

tealet_t *worker(tealet_t *current, void *arg) {
    printf("Hello from coroutine!\n");
    return current->main;  /* Return to caller */
}

int main(void) {
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main = tealet_initialize(&alloc, 0);
    
    void *arg = NULL;
    tealet_t *coro = tealet_new(main, worker, &arg);
    
    tealet_finalize(main);
    return 0;
}
```

Compile: `gcc -o example example.c -Isrc -Lbin -ltealet`

# Functionalty
The library provides the basic mechanism to create coroutine and to switch between them.
It takes care of allocating and saving the stack for dormant coroutines.  A way to pass simple values
between coroutines is provided but the user must be careful to pass any more complicated data on the
heap, and not via stack-local variables.

No form of scheduler is implemented.

## Advanced: Fork-like Semantics

In addition to the traditional approach where each tealet exists within the execution scope of a function (created via `tealet_new()` or `tealet_create()`), libtealet now supports **Unix-like fork semantics** through `tealet_fork()`.

This functionality was available in Stackless Python but has historically been omitted from this library to maintain the clean discipline of function-scoped coroutines. `tealet_fork()` breaks out of this restriction, enabling more dynamic coroutine creation patterns.

**Important responsibilities when using fork:**
- **Stack boundaries must be set**: Call `tealet_set_far()` to establish a stack boundary before forking, ensuring the forked execution doesn't exceed the promised scope
- **Explicit exit required**: Forked tealets have no run function to return from, so they **must** explicitly exit using `tealet_exit()` (without the `TEALET_FLAG_DEFER` flag)
- **Scope discipline**: All switching must occur within the bounded stack region defined by the `far` boundary

This feature enables advanced use cases like coroutine cloning and continuation capture, but requires careful management of stack boundaries and explicit lifetime control.

## Documentation

- **[Getting Started](docs/GETTING_STARTED.md)** - Installation, examples, common patterns
- **[API Reference](docs/API.md)** - Complete function reference
- **[Architecture](docs/ARCHITECTURE.md)** - Internals and stack-chaining design
- **[Changelog](CHANGELOG.md)** - Version history

## Performance Characteristics

| Metric | libtealet | OS Threads |
|--------|-----------|------------|
| Context switch | ~100-500 cycles | ~1000-10000 cycles |
| Memory per coroutine | ~2-16 KB (incremental) | 1-8 MB (fixed) |
| Creation overhead | ~1-2 µs | ~50-200 µs |
| Parallelism | No (single thread) | Yes (multi-core) |
| Scheduling | Manual | OS kernel |
| System calls | None (pure user-space) | Required |

**Trade-offs:**
- ✅ Much faster context switching (no kernel involvement)
- ✅ Lower memory overhead enables thousands of coroutines
- ✅ Deterministic scheduling (you control when switches happen)
- ❌ No true parallelism (runs on single OS thread)
- ❌ Manual scheduling required (no preemption)

# Example
For an example on how to implement `longjmp()` like functionality, as with the [`setcontext()` library](https://en.wikipedia.org/wiki/Setcontext)
see the file `tests/setcontext.c`

# History
The tealet code was originally extracted from the Python [Greenlet][greenlet]
project by Armin Rigo and the original version was written by him.  Armin had
previously created the Greenlet project by extracting the stack slicing code from
from [Stackless Python][stackless].

# Changelog
See [CHANGELOG.md](CHANGELOG.md) for release history and version information.

[stackman]: https://github.com/stackless-dev/stackman
[greenlet]: https://pypi.python.org/pypi/greenlet
[stackless]: http://www.stackless/com "Stackless Python"
[coroutine]: https://en.wikipedia.org/wiki/Coroutine "Coroutine"
[cpp20]: https://en.wikipedia.org/wiki/C%2B%2B20 "C++20"
[coroc]: https://en.wikipedia.org/wiki/Coroutine#Implementations_for_C "Coroutine Implementations for C"

