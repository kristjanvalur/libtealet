# Libtealet

**Version 0.1.0**

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

# Functionalty
The library provides the basic mechanism to create coroutine and to switch between them.
It takes care of allocating and saving the stack for dormant coroutines.  A way to pass simple values
between coroutines is provided but the user must be careful to pass any more complicated data on the
heap, and not via stack-local variables.

No form of scheduler is implemented.


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

