# Stack integrity feature evaluation (temporary report)

Date: 2026-03-26

## Scope

This report compares local debugging/safety changes in:

- `/home/kristjan/git/pytealet/src/_tealet/libtealet`

against this workspace:

- `/home/kristjan/git/libtealet`

with focus on:

1. compile-time integration/debug features,
2. run-time safety features,
3. a hybrid stack-integrity design using page protection + snapshot,
4. Linux tradeoffs between `mprotect()` and `userfaultfd`.

## Summary of what differs

Most divergence is in `src/tealet.c` in the pytealet copy (large local debug/safety instrumentation), with small public API differences in `src/tealet.h`.

Key local additions observed:

- Linux page-guard mode with `mprotect(PROT_NONE)` around switch boundaries.
- Guard state tracked in main tealet internals.
- Signal-handler diagnostics for guard faults (`SIGSEGV`/`SIGBUS`).
- Optional full initial stack snapshot + `memcmp()` validation logic.
- Optional pre-restore consistency assertions.
- Toggle macros for diagnostics and behavior experiments.
- API addition in local copy: `tealet_set_page_guard(tealet_t *main, int enabled)`.

## Classification

### A) Compile-time features for integration/debugging

Recommended to include (all default OFF):

- `TEALET_WITH_STACK_GUARD`:
  - Enables platform backends for guard checks.
  - Linux backend can use `mprotect()`.
- `TEALET_WITH_STACK_SNAPSHOT_VERIFY`:
  - Enables boundary snapshot/verify mechanism.
- `TEALET_WITH_STACK_GUARD_DIAGNOSTICS`:
  - Enables extra logging/assert-heavy diagnostics.

Not recommended as productized features (keep local/private unless needed):

- Magic-cookie macros used for ad-hoc corruption probing.
- Force-full-save debug macro.
- Very broad assertion bundles that materially alter hot paths.

Reason: these are useful for deep debugging but are not good long-term API surface.

### B) Run-time safety features

Recommended:

- One runtime configuration API (e.g. `tealet_configure()` on main tealet) controlling:
  - enable/disable integrity checks,
  - guard mode (`none`, `read-only`, `no-access`),
  - a single `stack_integrity_bytes` window above boundary (implementation rounds to whole pages for guard coverage, using snapshot for remaining partial coverage),
  - failure policy (`assert`, error return, abort/log).

This gives safe defaults (off), and opt-in safety without recompilation when compile-time support is present.

## Feasibility of hybrid design (`mprotect` + snapshot)

Proposed behavior (feasible and aligned with current switch architecture):

1. Compute protected page-aligned range above current tealet far boundary.
2. Apply guard to first `N` whole pages (runtime configurable).
3. Snapshot the first partial non-page-aligned region above the boundary.
4. On switch-back, verify snapshot before restore/switch completion.

Why this works well:

- `mprotect` gives strong faulting semantics on whole pages.
- Snapshot closes the page-alignment gap (the first partial page that cannot be isolated cleanly with page granularity).
- Design supports a Windows snapshot-only backend naturally.

Suggested platform policy:

- Linux: `mprotect` + snapshot.
- Windows: snapshot-only (fixed byte window, e.g. 1024 B to configurable).
- Other platforms: snapshot-only fallback.

## Cost estimate

### Engineering cost (implementation)

Estimated effort: **2–4 engineering days** for a clean, upstreamable first version:

- config API + plumbing,
- Linux `mprotect` backend,
- snapshot backend,
- tests/documentation,
- failure-mode behavior and diagnostics.

Could expand to **4–7 days** if adding:

- richer per-platform behavior,
- extensive compatibility matrix,
- benchmarking harness and detailed stats counters.

### Runtime cost

1. **Compiled in but disabled at runtime**
   - Near-zero overhead (branch checks only in switch path).

2. **Snapshot-only enabled**
   - Low overhead: copy + compare of configured byte window per switch boundary.
   - With 1 KiB window this is typically small compared to syscall-based guards.

3. **`mprotect` guard enabled**
   - Moderate to high overhead for high-frequency switching:
     - extra protection transitions around switches,
     - kernel work and potential TLB impact,
     - can dominate tight coroutine-switch loops.
   - Best used for debugging/integration/safety-sensitive deployments rather than max-throughput mode.

## Linux tradeoff: `mprotect()` vs `userfaultfd`

## `mprotect()`

Pros:

- Simple, direct fit for this codebase.
- Synchronous fault behavior via normal protection violation.
- No dedicated monitor thread or fd event loop required.

Cons:

- Page granularity only.
- Repeated protection changes can be expensive in hot switch loops.
- Signal handling can be intrusive if custom diagnostics intercept faults.

## `userfaultfd`

Pros:

- Rich, programmable fault handling model.
- Write-protect mode exists for write tracking semantics.
- Potentially cleaner separation of fault processing from signal handlers.

Cons:

- Much higher implementation complexity:
  - syscall/ioctl handshake,
  - registration lifecycle,
  - event loop/handler thread synchronization.
- Permission and deployment constraints (e.g., unprivileged restrictions via `vm.unprivileged_userfaultfd`).
- Kernel-version/feature variability and broader maintenance burden.
- For this specific use case, complexity likely outweighs benefit.

Recommendation for Linux:

- Start with `mprotect` backend + snapshot.
- Do **not** adopt `userfaultfd` in first iteration.
- Revisit `userfaultfd` only if you need advanced event-driven memory-fault workflows beyond stack-boundary guarding.

## Recommended rollout

1. Land compile-time gates (off by default).
2. Add runtime configuration API (off by default).
3. Implement snapshot-only mode first (cross-platform baseline).
4. Add Linux `mprotect` backend guarded by build/platform checks.
5. Add focused benchmarks:
   - switch throughput with checks off/on,
  - impact of `stack_integrity_bytes` window size.

## Proposed `tealet_configure` API draft

Proposed public API shape:

- `int tealet_configure_get(tealet_t *tealet, tealet_config_t *config);`
- `int tealet_configure_set(tealet_t *tealet, tealet_config_t *config);`

`tealet_configure_set()` should canonicalize in-place to the effective applied configuration, leaving only features supported by the current build/platform enabled.

Proposed compatibility strategy:

- `tealet_config_t` starts with:
  - `size_t size;`
  - `unsigned int version;`
- `version` is **struct-format version**, not global lib ABI version.
- Define `TEALET_CONFIG_VERSION_1` as initial value and use a macro alias for current version.
- Implementations read/write only the known prefix up to `min(config->size, sizeof(tealet_config_t))`.
- Unknown trailing fields are ignored (forward compatibility).

Proposed v1 payload fields:

- `flags` (enable stack integrity / guard / snapshot modes),
- `stack_integrity_bytes`,
- `stack_guard_mode` (`none`, `read-only`, `no-access`),
- `stack_integrity_fail_policy` (`assert`, error return, abort),
- reserved array for future expansion without immediate struct bump.

## Proposed initial defaults

If enabled at runtime without explicit tuning:

- guard mode: `no-access` (Linux backend),
- `stack_integrity_bytes`: `8192` (example default; effectively one or two pages on most systems),
- failure policy: `assert` in debug builds, error return in release builds.

## Practical recommendation

Adopt a **hybrid, staged safety feature**:

- compile in support optionally,
- keep runtime disabled by default,
- use snapshot-only as portable baseline,
- offer Linux `mprotect` guard for strong debugging/safety checks,
- avoid `userfaultfd` in initial implementation due to complexity/cost mismatch.

---

Temporary report file; intended for design discussion and iteration.
