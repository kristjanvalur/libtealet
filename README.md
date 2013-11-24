Tealet
===================

The tealet library is a C and assembler implemantation of stack slicing
for various platforms.  It has no external dependencies, apart from
`memcpy()` from the C runtime, and even this function is most commonly
inlined by the compilers.

(Debug builds will use `assert()` that do rely on other parts of the runtime.)

The library can thus be used to add stack slicing to any C application.

Tealet
-------------------
This is the raw tealet project.  It was originally extracted from the [Greenlet][1]
project by Armin Rigo.  Greenlet was in turn distilled from [Stackless][2]

Various platforms are supported.  Full thread-safe switching is provided for some of
them, but others only have Stackless-style switching, which means that they rely on
global variables during stack slicing and therefore cannot be concurrently used on
multiple threads in a single application.

PyTealet
---------------------
This is a python module for Python 2.7 providing the tealet switching functionality.
It is more primitive than the features provided by [Greenlets][1] and so an emulation
module is also provided.


[1]: https://pypi.python.org/pypi/greenlet "Greenlets"
[2]: http://www.stackless/com "Stackless Python"