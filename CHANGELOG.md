# Changelog

All notable changes to libtealet will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/kristjanvalur/libtealet/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/kristjanvalur/libtealet/releases/tag/v0.1.0
