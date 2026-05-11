# Changelog

All notable changes to libtealet will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **NOFAIL transfer policies for both exit and switch APIs**
  - Added `TEALET_XFER_NOFAIL` to provide a
    robustness-oriented transfer mode for callers that prioritize forward
    progress under memory pressure/defunct-target conditions.
  - NOFAIL attempts the requested transfer with FORCE first.
  - On expected runtime transfer failures (`TEALET_ERR_MEM` /
    `TEALET_ERR_DEFUNCT`), NOFAIL falls back to `PANIC|FORCE` transfer to main.
  - Other errors (for example `TEALET_ERR_INVAL`, and `TEALET_ERR_PANIC` on
    switch) are returned unchanged.

### Changed
- **Regression test suite was restructured into focused modules**
  - Split the former monolithic regression harness into dedicated test files
    (lifecycle, transfer, locking, resilience, stack, stats, and stress) with
    shared harness helpers.
  - Preserved existing behavior while improving maintainability and
    cross-platform project integration.

- **Transfer internals now use one-shot signaling flags with explicit consumption points**
  - Renamed the internal transient per-tealet force bit from `EXITFORCE` to
    `SAVEFORCE` to reflect that it applies to save behavior generally, not only
    exit flows.
  - Clarified internal ownership/lifecycle: transfer prep sets one-shot signals
    (`SAVEFORCE`, `PANIC`) before the handoff, and low-level transfer helpers
    consume and clear them during the same operation.

- **Switch/exit transfer paths are now factored around shared kinship**
  - Refactored `tealet_switch()` and `tealet_exit()` to leverage a shared
    internal transfer helper (`tealet_xfer_inner`) for common transfer
    mechanics, while preserving each API's mode-specific behavior.
  - Kept mode-specific semantics explicit (for example exit-mode invariants and
    retry policy orchestration) while reducing duplicate transfer plumbing.

- **Run-return lifecycle now keeps tealets alive by default**
  - Returning from a tealet run function now follows default non-delete exit
    semantics (equivalent to `TEALET_XFER_DEFAULT` transfer behavior for
    `tealet_exit()`), so the tealet remains allocated instead of being
    implicitly deleted.
  - Automatic deletion remains available via explicit
    `tealet_exit(..., TEALET_EXIT_DELETE)`.
  - Tests and examples were updated to perform explicit `tealet_delete()`
    cleanup where ownership remains with the caller.
  - API/docs now clarify that `TEALET_EXIT_DELETE` invalidates outstanding
    pointers to the exiting tealet after transfer.

- **Creation semantics now use explicit allocation + bind/start steps**
  - `tealet_new()` now allocates and returns a NEW/unbound tealet handle.
  - `tealet_run()` is the bind/start primitive, with:
    - `TEALET_RUN_DEFAULT` for deferred start (capture without immediate switch)
    - `TEALET_RUN_SWITCH` for immediate start via an optimized single path
      equivalent in effect to `TEALET_RUN_DEFAULT` + `tealet_switch()`.
  - `tealet_fork()` now uses `TEALET_RUN_DEFAULT` / `TEALET_RUN_SWITCH`
    mode flags.

- **Public transfer flags were renamed to `TEALET_XFER_*`**
  - User-facing switch/exit transfer behavior now uses shared
    `TEALET_XFER_*` names.
  - `tealet_switch()` now uses:
    - `TEALET_XFER_FORCE` (was `TEALET_SWITCH_FORCE`)
    - `TEALET_XFER_PANIC` (was `TEALET_SWITCH_PANIC`)
    - `TEALET_XFER_NOFAIL` (was `TEALET_SWITCH_NOFAIL`)
  - `tealet_exit()` now uses:
    - `TEALET_XFER_FORCE` (was `TEALET_EXIT_FORCE`)
    - `TEALET_XFER_PANIC` (was `TEALET_EXIT_PANIC`)
    - `TEALET_XFER_NOFAIL` (was `TEALET_EXIT_NOFAIL`)
    - plus exit-only `TEALET_EXIT_DELETE` / `TEALET_EXIT_DEFER`.
  - `TEALET_XFER_*` occupies the low 8-bit transfer flag space; exit-only flags
    begin at `0x100` to reserve headroom for future transfer-flag growth.

- **Build now tracks generated header dependencies automatically**
  - Make rules now emit and include compiler-generated dependency files for C
    objects.
  - Header edits now trigger the correct object rebuilds without requiring a
    manual clean step.

### Removed
- **Legacy creation API surface removed from core**
  - Removed core `tealet_create()` and the old create-and-start `tealet_new(...)`
    out-parameter signature.
  - Removed legacy `TEALET_FORK_DEFAULT` / `TEALET_FORK_SWITCH` flags in favor
    of `TEALET_RUN_*`.

- **Separate switch/exit transfer-flag names removed**
  - Removed `TEALET_SWITCH_DEFAULT` / `TEALET_SWITCH_FORCE` /
    `TEALET_SWITCH_PANIC` / `TEALET_SWITCH_NOFAIL`.
  - Removed `TEALET_EXIT_DEFAULT` / `TEALET_EXIT_FORCE` /
    `TEALET_EXIT_PANIC` / `TEALET_EXIT_NOFAIL`.

### Documentation
- Updated `README.md`, `docs/GETTING_STARTED.md`, `docs/API.md`,
  `docs/ARCHITECTURE.md`, and `src/tealet.h` Doxygen comments to reflect the
  new creation flow and run-mode semantics.

## [0.6.0] - 2026-05-09

### Changed
- **Switch/exit behavior is now explicit via flags**
  - `tealet_switch()` now takes a `flags` parameter and supports:
    - `TEALET_SWITCH_DEFAULT`
    - `TEALET_SWITCH_FORCE`
    - `TEALET_SWITCH_PANIC`
  - `TEALET_SWITCH_FORCE` / `TEALET_EXIT_FORCE` semantics are now documented explicitly, including the main-stack edge case where `TEALET_ERR_MEM` can still occur under FORCE.

- **Creation APIs now return status codes with out-parameters**
  - `tealet_new()` and `tealet_create()` now return `int` status and report created tealets via out-pointers.
  - `tealet_stub_new()` in extras/helpers follows the same status-return pattern.
  - Call sites and examples were migrated to the status-first style.

- **Implicit run-return exit policy hardened and documented**
  - Automatic exit on run-function return now uses an explicit retry/fallback policy:
    - requested target (default)
    - panic+force to main on defunct target
    - requested target with FORCE on memory failure
    - panic+force to main if FORCE still cannot complete transfer
  - API docs now include a concise robust manual `tealet_exit()` pattern for implementors.

- Updated bundled Stackman distribution to `v1.2.1`.

### Documentation
- Updated `README.md`, `docs/API.md`, `docs/ARCHITECTURE.md`, and `docs/GETTING_STARTED.md` to reflect the new signatures and behavioral semantics.

## [0.5.1] - 2026-05-08

### Added
- **Switch-scoped lock mode configuration**
  - Added explicit lock mode selection in `tealet_lock_t.mode`:
    - `TEALET_LOCK_OFF`
    - `TEALET_LOCK_SWITCH`

### Changed
- **Locking behavior is now mode-driven**
  - `TEALET_LOCK_SWITCH` enables internal auto-locking for switching APIs only (`tealet_new()`, `tealet_create()`, `tealet_switch()`, `tealet_exit()`, `tealet_fork()`).
  - `TEALET_LOCK_OFF` leaves lock ownership fully to integrator-managed scopes.
  - Updated tests/helpers to configure lock mode explicitly where lock-callback behavior is validated.

### Documentation
- **Locking model and API docs refresh**
  - Expanded and reorganized `docs/API.md` to clarify lock-mode semantics and switching-vs-manual locking expectations.
  - Refined README/API split: README remains concise and points to `docs/API.md` as the canonical behavioral reference.
  - Added release-version synchronization guidance and tooling notes (`make sync-version`, `make check-version-sync`) to reduce metadata drift.

## [0.5.0] - 2026-05-07

### Added
- **Locking callback API for synchronized foreign-thread structure access**
  - Added `tealet_lock_t` callback descriptor.
  - Added `tealet_config_set_locking()` to configure lock/unlock callbacks per main-tealet domain.
  - Added utility entry points `tealet_lock()` and `tealet_unlock()`.
- **Incremental save algorithm documentation**
  - Added `docs/INCREMENTAL_SAVE.md` describing partial-save invariants and transition behavior.

### Changed
- **Automatic lock ownership is now limited to core switching APIs**
  - Internal auto-lock scope is centered on:
    - `tealet_new()`
    - `tealet_create()`
    - `tealet_switch()`
    - `tealet_exit()`
    - `tealet_fork()`
  - Non-switch APIs remain caller-synchronized where foreign-thread access is possible.

### Fixed
- **Unreachable exit fallback hardening**
  - Hardened the unreachable `tealet_exit()` return path to fail fast via `abort()` in release builds.

## [0.4.3] - 2026-04-22

### Added
- **Tealet origin flags API**
  - Added `tealet_get_origin()` to expose tealet origin/lineage flags.
  - Added public origin bits:
    - `TEALET_ORIGIN_MAIN_LINEAGE`
    - `TEALET_ORIGIN_FORK`
  - Added convenience macros:
    - `TEALET_IS_MAIN_LINEAGE()`
    - `TEALET_IS_FORK()`
  - Origin bits are tracked on existing per-tealet internal flags and preserved across relevant clone paths.

### Changed
- **Fork semantics documentation clarity**
  - Clarified in `src/tealet.h`, `README.md`, and `docs/API.md` that main-lineage fork caveats are scoped to main-lineage execution (main or clone-of-main), not all forks.
  - Clarified that forked children inherit the parent/current far boundary (`stack_far`) at fork time.

### Tests
- **Origin flag coverage**
  - Added origin-flag assertions directly into existing `tealet_fork()` behavior tests in `tests/test_fork.c`.
  - Added a regular non-fork tealet assertion path in `tests/tests.c` to verify baseline origin classification.

## [0.4.2] - 2026-04-04

### Added
- **Panic reroute switch error (`TEALET_ERR_PANIC`)**
  - Added `TEALET_ERR_PANIC` as a switch result in `main` when `tealet_exit()` reroutes to `main` because the requested exit target is defunct.
  - Added internal panic-latch handling in switch/exit flow using an internal main-flag bit.
- **Testing gate for internal hooks**
  - Added `TEALET_WITH_TESTING` build knob (default `0`) propagated via Makefile.
  - Added internal test-only hook `tealet_debug_force_defunct()` guarded by `TEALET_WITH_TESTING`.

### Changed
- **Switch caller sanity checks**
  - Added caller-context verification in stack-switch paths to catch invalid caller/thread-stack usage earlier.
  - Added and documented `tealet_config_t.max_stack_size` as caller-switch-sanity-only control:
    - default `TEALET_DEFAULT_MAX_STACK_SIZE` (16 MiB)
    - non-zero enforces caller stack-distance bound
    - `0` removes stack-size assumptions by disabling this caller-distance validation
- **Error-handling API docs**
  - Expanded `docs/API.md` with dedicated error-handling guidance for `TEALET_ERR_MEM`, `TEALET_ERR_DEFUNCT`, and `TEALET_ERR_PANIC`.
  - Documented practical recovery guidance for main vs non-main contexts.
  - Clarified `tealet_new()` conceptual equivalence to `tealet_create()` + `tealet_switch()` and aligned failure semantics.
- **State model cleanup for exited tealets**
  - Switched exited-state representation to `TEALET_TFLAGS_EXITED` instead of relying on `stack_far == NULL` sentinel behavior.
  - Tightened related invariants in exit/switch paths.
- **Public API layout consistency**
  - Reorganized declaration/definition order in `src/tealet.h` and `src/tealet.c` by function group:
    core lifecycle/switching → status/query → configuration → utility.
  - Kept testing-only debug hooks grouped at file end under `TEALET_WITH_TESTING` in `src/tealet.c`.
- **In-code documentation conventions**
  - Expanded declaration-level Doxygen docs in `src/tealet.h` for core/status/config/utility APIs.
  - Adopted selective documentation style: declaration-level Doxygen in header; implementation-focused comments in `src/tealet.c`.
  - Normalized section banners to unambiguous non-Doxygen form in both `src/tealet.h` and `src/tealet.c`.

### Fixed
- **Defunct delete safety**
  - `tealet_delete()` now avoids decref on defunct marker state (`stack == (tealet_stack_t *)-1`).

### Tests
- **Panic reroute regression coverage**
  - Added regression test for defunct-target exit reroute signaling (`TEALET_ERR_PANIC`) under `TEALET_WITH_TESTING=1`.

## [0.4.1] - 2026-03-31

### Changed
- **Helper module rename**
  - Introduced `tealet_extras.h` / `tealet_extras.c` as the primary helper module names.
  - Updated build, tests, and API docs to use `tealet_extras.h`.
  - Kept `tools.h` as a compatibility alias header.

### Added
- **`tealet_previous()` API docs**
  - Added explicit API reference coverage for `tealet_previous()`.
  - Documented `NULL` return semantics when no previous tealet exists.
  - Documented `NULL` return semantics when the previously recorded tealet has been destroyed.

### Fixed
- **`g_previous` stale pointer on tealet destruction**
  - Clearing `g_previous` when the previously recorded tealet is deleted.
  - Applies to both automatic deletion on tealet completion and explicit `tealet_delete()`.
  - Added regression tests for both paths.

---

## [0.4.0] - 2026-03-28

### Summary
Major feature release introducing runtime **stack validation** for tealets, combining byte-level snapshot verification with page-guard enforcement where supported.

### Added
- **Stack validation runtime support**
  - Added configuration-driven stack validation using integrity checks with guard and snapshot backends
  - Added `tealet_configure_get()` / `tealet_configure_set()` APIs for explicit runtime control
  - Added mismatch policy handling via `TEALET_STACK_INTEGRITY_FAIL_*`
- **Stack-check convenience API**: Added `tealet_configure_check_stack(tealet_t*, size_t)`
  - Enables integrity + guard + snapshot checks with a single call
  - Uses `TEALET_STACK_GUARD_MODE_NOACCESS` and `TEALET_STACK_INTEGRITY_FAIL_ERROR`
  - Uses one page by default when `stack_integrity_bytes == 0` (Linux page size when available, fallback 4096)

### Changed
- **Default build now includes stack-check backends**
  - Build defaults enable `TEALET_WITH_STACK_GUARD=1` and `TEALET_WITH_STACK_SNAPSHOT=1`
  - Feature macros remain overridable at build time (for reduced builds)
- **Test coverage with checks enabled by default**
  - Enabled stack checks in core and selected test harnesses (`tests.c`, `test_fork.c`, `test_stochastic.c`, `test_chunks.c`, `setcontext.c`)
  - `test_config.c` now separates plain-init API-shape tests from checks-enabled runtime behavior tests
- **Documentation updates for stack checks**
  - Added README overview for optional stack checks and build-time opt-out flags
  - Added API reference coverage for `tealet_configure_get()`, `tealet_configure_set()`, and `tealet_configure_check_stack()`
- **Tools helper API update**: `tealet_stub_new()` now takes a `stack_far` argument
  - New signature: `tealet_stub_new(tealet_t *tealet, void *stack_far)`
  - Aligns stub creation boundary behavior with `tealet_new()` and `tealet_create()`
  - Updated stub call sites in tests to pass `NULL` where default behavior is desired
- **Tools helper documentation**
  - Added API reference section for `tools.h` helper extensions
  - Documented `tealet_statsalloc_init()`, `tealet_stub_new()`, and `tealet_stub_run()` as helper layers over core tealet APIs
- **Source formatting**
  - applied `clang-format` to the source code

### Fixed
- **Protected-stack argument bug in `tealet_new()` path**
  - Fixed a crash where `tealet_initialstub()` dereferenced `*parg` after switching into a new tealet with stack guards active
  - `tealet_new()` now captures the initial argument value before stack switching
  - This preserves previously valid usage patterns under stack-protection mode

- **Breaking API change for tealet creation**: `tealet_new()` and `tealet_create()` now require an additional `stack_far` parameter.
  - New signatures:
    - `tealet_new(tealet_t *main, tealet_run_t run, void **parg, void *stack_far)`
    - `tealet_create(tealet_t *main, tealet_run_t run, void *stack_far)`
  - Migration: pass `NULL` as `stack_far` to preserve previous behavior.
- **Renamed probe helper**: `tealet_new_far()` is replaced by `tealet_new_probe()`.

## [0.3.2] - 2025-11-22

### Fixed
- **`tealet_previous()` correctness for forked tealets**: Fixed bug where forked child tealets
  would not correctly see their parent as the previous tealet when first switched to
  - In `TEALET_FORK_DEFAULT` mode, `tealet_switchstack()` returns twice (parent and child contexts)
  - Previously, `g_previous` was unconditionally restored after the switch, clearing it in the child
  - Now only restores `g_previous` in parent context (when `result == 1`)
  - Child correctly sees parent as previous tealet, parent maintains original previous value

### Added
- **Comprehensive `tealet_previous()` tests**: Added tests verifying correct previous tealet tracking
  - `test_basic_fork()`: Enhanced to verify `tealet_previous()` after switch and exit
  - `test_fork_switch()`: Enhanced to verify `tealet_previous()` with `TEALET_FORK_SWITCH`
  - `test_new_previous()`: New test verifying `tealet_previous()` with `tealet_new()`
  - `test_create_previous()`: New test verifying `tealet_previous()` with `tealet_create()`

## [0.3.1] - 2025-11-22

### Summary
Refactoring release that centralizes internal state management and extends argument
passing to `tealet_fork()`. While this includes an API change to the experimental
`tealet_fork()` function, breaking changes are expected for this feature.

### Changed
- **Refactored internal state management**: Centralized stack switching logic
  - `tealet_switchstack()` now takes explicit `target`, `in_arg`, and `out_arg` parameters
  - State variables (`g_target`, `g_arg`) managed internally by `tealet_switchstack()`
  - Simplified calling functions: `tealet_new()`, `tealet_switch()`, `tealet_exit()`, `tealet_fork()`
  - Enhanced `tealet_initialstub()` to properly handle run function argument passing
  - Cleaner separation of concerns and more maintainable code structure
- **`tealet_fork()` API change**: Added `parg` parameter for argument passing
  - Signature: `int tealet_fork(tealet_t *current, tealet_t **pother, void **parg, int flags)`
  - With `TEALET_FORK_DEFAULT`: Passes value to suspended child on first switch
  - With `TEALET_FORK_SWITCH`: Passes value to suspended parent when child switches back
  - Can be NULL if no argument passing is desired (similar to `tealet_new()` and `tealet_switch()`)
  - Enables bidirectional value passing between parent and child, consistent with other tealet operations

### Added
- **Fork argument passing tests**: Two new comprehensive tests in `tests/test_fork.c`
  - `test_fork_switch_arg()`: Verifies argument passing with `TEALET_FORK_SWITCH`
  - `test_fork_default_arg()`: Verifies bidirectional argument passing with `TEALET_FORK_DEFAULT`
  - Tests validate proper isolation of stack variables between parent and child

### Documentation
- Updated `tealet_fork()` documentation in `src/tealet.h` to explain `parg` parameter
- Updated API.md with new signature and argument passing examples
- Added note that `parg` can be NULL (consistent with `tealet_new()` and `tealet_switch()`)

## [0.3.0] - 2025-11-20

### Summary
Major feature release adding Unix-like fork semantics for tealets, enabling dynamic
coroutine cloning at any execution point. Includes comprehensive testing suite and
statistics API for memory tracking.

### Added
- **Fork-like semantics**: New `tealet_fork()` function for Unix-like fork behavior
  - Creates a child tealet by duplicating the current execution state
  - Two modes: `TEALET_FORK_DEFAULT` (child suspended) and `TEALET_FORK_SWITCH` (immediate switch to child)
  - Returns 0 in child, 1 in parent (similar to Unix fork)
  - Symmetric `pother` parameter allows both parent and child to reference each other
  - **Breaking the function-scope discipline**: Unlike `tealet_new()` which creates tealets within function scope, `tealet_fork()` enables dynamic coroutine cloning at any point
  - **Important**: Forked tealets must use `tealet_exit()` explicitly (without `TEALET_EXIT_DEFER`) since they have no run function to return from
- **Stack boundary API**: New `tealet_set_far()` function
  - Sets a stack boundary on the main tealet
  - Required before calling `tealet_fork()` to prevent two unbounded stacks
  - Enables bounded stack execution for the main tealet
- **Comprehensive fork test suite**: `tests/test_fork.c` with 4 tests
  - Basic fork and switch patterns
  - Multiple fork scenarios
  - Parent-child ping-pong switching
- **Memory statistics API**: New `tealet_get_stats()` function for tracking memory usage
  - Tracks allocated bytes and blocks (current and peak)
  - Counts active tealets
  - Stack memory metrics: bytes, stacks, chunks
  - Memory efficiency tracking: actual vs naive allocation
  - Expanded vs naive byte comparisons for multi-chunk analysis
- **Statistics reset API**: `tealet_reset_peak_stats()` to reset peak counters
- **Test suite enhancements**: New comprehensive tests
  - `test_chunks.c`: Validates multiple chunks per stack and chunk tracking
  - `test_stochastic.c`: Realistic usage simulation with stochastic behavior
    - Dynamic tealet lifecycle (create/exit during execution)
    - Recursive workers with random decisions (recurse/return/switch/spawn/exit)
    - Two shutdown modes: clean (unwind stacks) and immediate (delete active)
    - Command-line configurable (operations, depth, verbose mode)
    - Demonstrates statistics API and voluntary exit patterns

### Changed
- **Renamed macro**: `TEALET_IS_MAIN_STACK` → `TEALET_STACK_IS_UNBOUNDED`
  - Clarifies that it checks for unbounded stack boundary, not main tealet identity
  - Critical distinction after fork allows main to have bounded stack
  - Added comprehensive documentation explaining the constraint
- **Renamed exit flag constants**: `TEALET_FLAG_*` → `TEALET_EXIT_*`
  - New names: `TEALET_EXIT_DEFAULT`, `TEALET_EXIT_DELETE`, `TEALET_EXIT_DEFER`
  - Old names (`TEALET_FLAG_*`) retained for backwards compatibility
  - Clearer semantic meaning aligned with exit functionality
- **Fixed assertions**: Corrected checks to distinguish between main tealet structure and unbounded stack behavior
  - `tealet_new()` and `tealet_create()`: Removed bogus unbounded stack checks
  - Exit handling: Check for main tealet structure, not unbounded stack
  - Defunct marking: Protect main tealet from being marked invalid
- **Protected main tealet**: Added safeguard in `tealet_stack_growto()`
  - Prevents main's stack from being marked defunct during exit fallback
  - Returns failure instead, triggering redirect-to-main logic in `tealet_exit()`
  - Ensures main tealet integrity even in out-of-memory scenarios
- **Statistics enabled by default**: `TEALET_WITH_STATS=1` now defined in Makefile
- **Test suite integration**: All new tests integrated into `make test` target

### Documentation
- Added "Advanced: Fork-like Semantics" section to README.md
- Documented fork responsibilities: stack boundaries, explicit exit, scope discipline
- Updated API documentation for `tealet_fork()` with detailed usage notes
- Clarified that forked tealets break the traditional function-scope discipline

## [0.2.0] - 2025-11-17

### Summary
Major distribution improvement: bundle pre-built stackman libraries for all platforms,
eliminating the git submodule dependency and simplifying both building and distribution.
Added comprehensive CI/CD pipeline for automated multi-platform builds and testing.

### Added
- **Bundled stackman distribution v1.2.0**: Pre-built libraries for 10 platform configurations
  - Linux: AMD64, i386, ARM32, ARM64, RISC-V64
  - macOS: x86_64 (Intel), ARM64 (Apple Silicon)  
  - Windows: x86, x64, ARM64
- **GitHub Actions CI/CD workflow**: Automated builds and tests for all platforms
  - Cross-compilation support with QEMU emulation for ARM and RISC-V
  - Platform-specific test execution (native only)
  - Artifact uploads for release automation
- **Self-contained libraries**: Both static and shared libraries now include stackman
  - `libtealet.a`: Merged static library (ar extract + merge)
  - `libtealet.so`/`.dylib`: Stackman statically linked into shared library
  - `tealet.dll`: Windows DLL with stackman built in
- **Visual Studio ARM64 support**: Full Windows ARM64 build configurations
- **Release workflow**: Automated tarball creation with all platform libraries

### Changed
- **Removed git submodule**: Replaced stackman submodule with bundled distribution
- **Simplified linking**: Users only need `-ltealet` (no separate `-lstackman`)
- **Makefile improvements**: Auto-detection of platform ABI, cross-compilation support
- **Windows output directories**: Standardized to `$(Platform)\$(Configuration)` pattern
- **Visual Studio project paths**: Converted absolute paths to relative paths
- **.gitignore**: Updated to allow bundled stackman libraries in `stackman/lib/`

### Fixed
- **Windows x64 compiler warnings**: Fixed C4244 warnings (intptr_t to int conversions)
- **Windows linker warnings**: Changed EditAndContinue to ProgramDatabase for Win32
- **Visual Studio project XML**: Fixed multiple XML syntax errors in .vcxproj files
- **macOS static linking**: Disabled -static flag (not supported on macOS)
- **QEMU emulation**: Added sysroot paths for dynamic binary execution
- **Build artifacts**: Create bin/ directory before building
- **Cross-compilation**: Proper PLATFORMFLAGS handling in CFLAGS and LDFLAGS

### Removed
- **Travis CI configuration**: Replaced with GitHub Actions
- **Empty switch.c file**: Obsolete file removed

### Documentation
- Added comprehensive style guide for documentation
- Updated README with bundled distribution approach
- GitHub Copilot instructions for project context

### Platform Support
Unchanged from v0.1.0, but now with pre-built libraries for all platforms.

### Dependencies
- **stackman v1.2.0** (bundled, no longer a git submodule)
- Standard C library (minimal dependencies unchanged)

## [0.1.0] - 2025-11-16

### Summary
Baseline release establishing v0.1.0 as the starting point for semantic versioning.
This version captures the current stable state of libtealet with all features developed
since the project's inception in 2013.

### Features
- Stack-slicing based coroutines for C (no compiler support required)
- Custom memory allocator support via `tealet_alloc_t` interface
- Tealet lifecycle management: create, switch, duplicate, delete
- Extra data allocation per tealet via `extrasize` parameter
- Platform-specific stack operations via stackman library dependency
- Helper utilities for stack arithmetic and tealet status queries
- Comprehensive test suite in `tests/tests.c`
- Example implementation of setcontext-like functionality in `tests/setcontext.c`

### Platform Support
- Linux (x86_64, ARM64, ARM32, RISC-V64)
- macOS (x86_64, ARM64)
- Windows (x86, x86_64, ARM, ARM64)

### Dependencies
- stackman library for low-level stack operations
- Standard C library: `memcpy()`, `malloc()`/`free()` (replaceable)
- `assert()` (debug builds only)

### Documentation
- README.md with conceptual overview and stack-slicing explanation
- API documentation in header file comments
- Example code demonstrating usage patterns

### Pre-History (2013-2025)
This release represents the accumulated work since the project's creation:
- 2013-04: Initial project files, extracted from Python Greenlet
- 2013-04: Platform support for GCC and MSVC
- 2013-04: Extra data support for user structures
- 2013-04: Statistics and memory tracking utilities
- 2013-05: Added Makefile build system
- 2013-05: Directory restructuring
- 2014-2015: Various bug fixes and improvements
- 2015-2024: Maintenance and compatibility updates
- 2024-11: Documentation improvements
- 2025-11: GitHub Copilot onboarding with copilot-instructions.md

[Unreleased]: https://github.com/kristjanvalur/libtealet/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/kristjanvalur/libtealet/compare/v0.5.1...v0.6.0
[0.5.1]: https://github.com/kristjanvalur/libtealet/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/kristjanvalur/libtealet/compare/v0.4.3...v0.5.0
[0.4.3]: https://github.com/kristjanvalur/libtealet/compare/v0.4.2...v0.4.3
[0.4.2]: https://github.com/kristjanvalur/libtealet/compare/v0.4.1...v0.4.2
[0.4.1]: https://github.com/kristjanvalur/libtealet/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/kristjanvalur/libtealet/compare/v0.3.2...v0.4.0
[0.3.2]: https://github.com/kristjanvalur/libtealet/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/kristjanvalur/libtealet/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/kristjanvalur/libtealet/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/kristjanvalur/libtealet/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/kristjanvalur/libtealet/releases/tag/v0.1.0
