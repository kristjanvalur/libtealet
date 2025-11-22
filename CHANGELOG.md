# Changelog

All notable changes to libtealet will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/kristjanvalur/libtealet/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/kristjanvalur/libtealet/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/kristjanvalur/libtealet/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/kristjanvalur/libtealet/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/kristjanvalur/libtealet/releases/tag/v0.1.0
