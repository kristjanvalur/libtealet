# Libtealet

LibTealet is a lightweight co-routine library for C.  It has no external
library dependencies except for memcpy().  It is based on the
technique of stack-slicing and contains C and assember implementation
for the most common modern hardware and OS platforms.

## Co-routines
In today's common parlance, co-routines refer to individual execution
contexts within a single operating system thread, which can pass control
and data between each other.  They are available as native constructs
in some higher level languages and also as libraries on some operating
systems.  For an overview, see 
https://en.wikipedia.org/wiki/Coroutine#Implementations_for_C

## Stack-slicing
The approach used here employs *stack-slicing*, a term
coined by Christian Tismer to desctibe the technique employed by
Stackless-Python.  
Instead of each co-routine having its own stack in virtual memory,
parts of the C stack that belong to different co-routines are
stored on the heap and restored to the stack as required.  

For each platform, a small piece of assembly code is required.
This code stores cpu registers on the stack, then calls functions to
save/restore the stack to or from the heap, and adjusts the stack pointer
as required.  Then the cpu state is restored from the restored stack
and a new co-routine is running.


## Example
