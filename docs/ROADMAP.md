# Development roadmap

## Todo

1. Add support (for the examples/getcontext wrapper use case) to create tealets via `tealet_run()` in caller-provided pre-allocated stack space, to enable makecontext-like execution without stack save/restore on switches when appropriate.
2. Add a fork provision (for the examples/getcontext wrapper use case) that lets a forked tealet replace/assume the identity of its parent on switch-back, so cleanup can be completed correctly from the forked continuation path.
3. Evaluate whether `TEALET_EXIT_DEFER` should also store the target tealet and ignore the run-function return target, so target validation/error checks are finalized at defer setup time.
