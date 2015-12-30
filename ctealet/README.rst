Tealet
===================

The Tealet library is a C and assembler implemantation of stack slicing
for various platforms.  It has no external dependencies, apart from
`memcpy()` from the C runtime, and even this function is most commonly
inlined by the compilers[*]_.

.. [*] Debug builds will use `assert()` that do rely on other parts of the runtime.

The library can thus be used to add stack slicing to any C application.

Tealets are derived from Greenlets, but don't have any Python dependancies and
their semantics are even more primitive than those of Greenlets, not 
having parent-child relationships and requiring explicit allocation and destruction.
In addition they aim to be fully thread safe: While it is only possible to switch
between tealets belonging to the same thread (stack), many such threads can exists
in the program and switch independently.

Various platforms are supported.  Full thread-safe switching is provided for some of
them, but others only have Stackless-style switching, which means that they rely on
global variables during stack slicing and therefore cannot be concurrently used on
multiple threads in a single application.

Files
-----
- `tealet.c`, `tealet.h` The main tealet code.
- `tools.c`, `tools.h` Additional code, such as an example allocator.
- `tests.c` Test code excercising the library.

History
-------
The tealet code was originally extracted from the Python Greenlet_
project by Armin Rigo and the original version was written by him.  Armin had
previously created the Greenlet project by extracting the stack slicing code from
from Stackless_ Python.

.. _Greenlet: https://pypi.python.org/pypi/greenlet
.. _Stackless:  http://www.stackless/com "Stackless Python"