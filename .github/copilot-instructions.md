# GitHub Copilot Instructions for libtealet

## Project Overview

**libtealet** is a lightweight coroutine library for C that implements co-operative multitasking using stack-slicing techniques. Unlike traditional coroutines that require special language support (async/await), libtealet suspends and restores entire execution stacks, allowing any C function to be suspended without special keywords or compiler support.

## Core Concepts

### Stack-Slicing
- The library saves and restores portions of the C execution stack to/from the heap
- Each coroutine (called a "tealet") can suspend its entire call stack and resume later
- Inspired by Stackless Python and the Python Greenlet project

### Dependencies
- **stackman**: Low-level stack operations library (git submodule)
- Minimal runtime dependencies: `memcpy()`, `malloc()` (replaceable), `assert()` (debug builds only)

## Architecture

### Main Components

1. **tealet.h / tealet.c**: Core coroutine implementation
   - `tealet_t`: Main coroutine structure
   - `tealet_run_t`: Function pointer type for coroutine entry points
   - Lifecycle: `tealet_initialize()` → `tealet_create()`/`tealet_new()` → `tealet_switch()` → `tealet_exit()`/`tealet_finalize()`

2. **tools.h / tools.c**: Helper utilities for the library

3. **switch.c**: Platform-specific stack switching code

### Key Data Structures

- **tealet_t**: User-visible coroutine structure
  - `main`: Pointer to the main (root) tealet
  - `extra`: Optional user data buffer (if extrasize specified during init)

- **tealet_alloc_t**: Custom memory allocator interface
  - Allows replacing malloc/free with custom allocators
  - Uses context pointer for allocator state

## API Patterns

### Initialization Pattern
```c
tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
tealet_t *main = tealet_initialize(&alloc, extrasize);
// ... use tealets ...
tealet_finalize(main);
```

### Creating and Switching
```c
// Pattern 1: Create then switch
tealet_t *g = tealet_create(main, run_func);
void *arg = my_data;
tealet_switch(g, &arg);

// Pattern 2: Create and switch atomically
tealet_t *g = tealet_new(main, run_func, &arg);
```

### Run Function Pattern
```c
tealet_t *my_run(tealet_t *current, void *arg) {
    // Do work...
    // Return the next tealet to execute (typically main)
    return current->main;
}
```

### Exiting Pattern
```c
// In run function:
tealet_exit(target, arg, TEALET_FLAG_DELETE);  // Delete current tealet
tealet_exit(target, arg, TEALET_FLAG_NONE);     // Keep tealet alive
tealet_exit(target, arg, TEALET_FLAG_DEFER);    // Defer to return statement
```

## Code Style & Conventions

### Naming
- Public API: `tealet_*` prefix
- Constants: `TEALET_*` uppercase
- Types: `*_t` suffix (e.g., `tealet_t`, `tealet_run_t`)
- Error codes: Negative integers (`TEALET_ERR_*`)
- Status codes: `TEALET_STATUS_*`
- Flags: `TEALET_FLAG_*`

### C Standards
- Written in C89/C90 compatible style
- Uses `stddef.h` for `size_t`, `ptrdiff_t`
- Platform-specific code isolated in stackman library

### Memory Management
- All allocations go through `tealet_alloc_t` interface
- Use `TEALET_ALLOC_MALLOC()` and `TEALET_ALLOC_FREE()` macros
- Tealets auto-freed when run function returns (unless TEALET_FLAG_DELETE not set)

### Error Handling
- Functions return negative error codes or NULL on failure
- `TEALET_ERR_MEM`: Memory allocation failed
- `TEALET_ERR_DEFUNCT`: Target tealet is corrupt
- Use `assert()` for debug builds

## Important Warnings

### Stack Data Safety
⚠️ **Never pass stack-allocated data between tealets via arg pointers**
- Stack data becomes invalid when switching contexts
- Always allocate shared data on the heap
- Use `tealet_malloc()` for tealet-owned allocations

### Threading
- Each thread must have its own main tealet
- Never switch between tealets from different threads
- Tealets must share the same main tealet to switch between each other

### Defunct Tealets
- A tealet becomes defunct if its run function returns incorrectly
- Switching to defunct tealets returns `TEALET_ERR_DEFUNCT`
- Use `tealet_status()` to check tealet state

## Testing

### Build and Test
```bash
make test              # Run all tests
make tests             # Build test binaries
CFLAGS=-g make test    # Debug build
CFLAGS="-O3 -flto" LDFLAGS="-O3 -flto" make test  # Optimized build
```

### Test Files
- `tests/tests.c`: Main test suite
- `tests/setcontext.c`: Example implementing setcontext-like functionality

## Platform Support

- Uses stackman library for platform-specific stack operations
- Supports common modern desktop platforms (Windows, Linux, macOS)
- Platform detection via `stackman/tools/abiname.sh`
- Windows: Uses DLL export/import (`TEALET_API` macro)

## Building

### Library Outputs
- `bin/libtealet.so`: Shared library
- `bin/libtealet.a`: Static library

### Compilation Flags
- `-fPIC`: Position-independent code
- `-Wall`: Enable warnings
- `-Isrc -Istackman/stackman`: Include paths

## Examples

See `tests/setcontext.c` for a practical example implementing `longjmp()`-like functionality using tealets.

## When Suggesting Code

1. Always use the tealet allocator interface, never direct malloc/free
2. Check return values (NULL or negative error codes)
3. Ensure run functions return a valid tealet pointer
4. Document any shared data that needs heap allocation
5. Use convenience macros: `TEALET_MAIN()`, `TEALET_IS_MAIN()`, `TEALET_EXTRA()`
6. Follow C89 style (declarations at block start, no C99/C11 features)
7. Consider thread safety in multi-threaded contexts

## Documentation Style Guide

### Tone and Voice
- **Direct and concise**: Avoid unnecessary verbosity; state facts clearly
- **Technical but accessible**: Explain complex concepts without condescension
- **Pragmatic focus**: Emphasize practical usage and real-world implications
- **Honest about limitations**: Clearly state experimental features, platform constraints, and known issues

### Structure Principles

#### README Structure
1. **Brief introductory paragraph**: What the library does in 2-3 sentences
2. **Key differentiator upfront**: Lead with what makes it unique
3. **Installation section**: Simple, clear commands with platform notes
4. **Features overview**: Bullet points or short sections highlighting capabilities
5. **Usage examples**: Working code snippets early in the document
6. **Detailed API reference**: Organized by category, not alphabetically
7. **Platform/compatibility information**: Clear support matrix
8. **History/credits**: Acknowledge origins and contributors

#### API Documentation
- Lead with a one-sentence summary
- Explain the "why" before the "how"
- Provide practical examples inline
- Use tables for comparing approaches or versions
- Include "When to use" guidance
- Note performance characteristics when relevant

### Formatting Conventions

#### Headers
- Use sentence case for headers (not title case)
- Main sections: `##` (two hashes)
- Subsections: `###` (three hashes)  
- Use descriptive names: "Getting Started" not "Usage"

#### Code Examples
- Always include complete, runnable examples
- Show both the problem and the solution
- Add comments explaining non-obvious parts
- Use realistic variable names, not `foo`/`bar`
- Demonstrate error handling when relevant

#### Lists
- Use bullet points (`-`) for features, benefits, characteristics
- Use numbered lists for step-by-step procedures
- Keep list items parallel in structure
- Start with verbs for action items

#### Emphasis
- **Bold** for important concepts, warnings, or key terms
- `backticks` for code, function names, filenames, parameters
- _Italics_ sparingly for subtle emphasis
- 🎯 Emoji occasionally for visual scanning (installation, warnings, performance notes)

### Specific Patterns

#### Version/Platform Compatibility
```markdown
### Platform Support
- Linux (x86_64, ARM64, ARM32, RISC-V64)
- macOS (x86_64, ARM64)
- Windows (x86, x86_64, ARM, ARM64)
```

#### Feature Descriptions
Format: Brief description → Use case → Code example → Notes/caveats
```markdown
### `function_name()` - Brief description

One-sentence summary of what it does and why it matters.

Use this when you need to [specific use case].

\`\`\`c
// Practical example
tealet_t *t = tealet_new(main, my_func, &arg);
\`\`\`

**Note:** Platform-specific behavior or limitations.
```

#### Warnings and Notes
```markdown
⚠️ **Warning:** Never pass stack-allocated data between tealets

> **Note:** For Python 3.10+, prefer the native implementation
```

#### Changelog Entries
Follow Keep a Changelog format:
- Group by: Added, Changed, Deprecated, Removed, Fixed, Security
- Start each item with component or area in **bold**
- Use present tense: "Add feature" not "Added feature" in headers
- Past tense in descriptions: "Fixed bug where..."
- Include issue/PR numbers when relevant

### Writing Principles

1. **Start with working code**: Show, don't just tell
2. **Explain the why**: Don't just document what something does—explain when to use it
3. **Progressive disclosure**: Basic usage first, advanced topics later
4. **Real-world context**: Use practical examples from actual use cases
5. **Compare when helpful**: "Unlike X, this library does Y"
6. **Be specific about trade-offs**: Performance vs. simplicity, memory vs. speed
7. **Update documentation with code**: Keep them in sync
8. **Link liberally**: Cross-reference related functions and concepts

### Technical Writing Style

- Use active voice: "The function saves the stack" not "The stack is saved"
- Prefer present tense: "This returns a pointer" not "This will return"
- Write in second person for instructions: "You can use" or imperative "Use"
- Keep sentences short and focused
- One idea per paragraph
- Use parallel structure in lists
- Define acronyms on first use
- Avoid jargon unless it's standard terminology in the field

### Anti-Patterns to Avoid

❌ Marketing speak ("revolutionary", "blazing fast", "next-generation")
❌ Vague statements without specifics ("improves performance significantly")
❌ Apologetic language ("This is just a simple library")
❌ Assumed knowledge without links to explanations
❌ Code examples that don't compile
❌ Outdated version numbers or platform info
❌ Tutorials that skip error handling
❌ Overuse of "key" prefix ("key features", "key benefits", "key differentiator")—be direct instead

### Examples from This Project

Good concise explanation (from README):
> "libtealet enables symmetric coroutines (co-stacks) in pure C without compiler support"

Good technical detail (from stackman):
> "When called, it will call callback function twice: First with opcode STACKMAN_OP_SAVE..."

Good practical guidance:
> "Use this either directly: ... or with a context manager: ..."
