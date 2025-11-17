# Changelog

All notable changes to libtealet will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/kristjanvalur/libtealet/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/kristjanvalur/libtealet/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/kristjanvalur/libtealet/releases/tag/v0.1.0
