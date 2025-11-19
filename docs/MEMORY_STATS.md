# Memory Statistics Feature

## Overview

This document describes the memory statistics tracking feature for libtealet, which allows measuring the memory efficiency of the stack-slicing approach compared to traditional fixed-size stack allocation.

## Motivation

Libtealet uses stack-slicing to save only the active portions of coroutine stacks to the heap, growing segments as needed. This is more memory-efficient than pre-allocating fixed-size stacks for each coroutine, but the actual savings depend on usage patterns.

The memory statistics feature provides concrete measurements to:
- Demonstrate the memory efficiency of stack-slicing
- Help users understand their actual memory usage
- Identify potential optimizations
- Provide benchmarking data for different workloads

## Compilation

Statistics are **enabled by default** in libtealet. The feature is controlled by the `TEALET_WITH_STATS` compile-time flag:

```bash
# Stats enabled (default)
gcc -o myapp myapp.c -ltealet

# Stats disabled (for minimal overhead)
gcc -DTEALET_WITH_STATS=0 -o myapp myapp.c -ltealet
```

When `TEALET_WITH_STATS=0`, all statistics code compiles to nothing with zero runtime overhead.

### Runtime Detection

Applications can detect whether statistics are enabled by calling `tealet_get_stats()` after initialization. If statistics are disabled, all fields will be zero:

```c
tealet_stats_t stats;
tealet_get_stats(main, &stats);
int stats_enabled = (stats.blocks_allocated > 0);  /* At least main tealet allocated */
```

## Strategy

### What We Track

Statistics are organized into three logical groups:

#### 1. Basic Counts
- **n_active**: Number of currently active tealets (including main)
- **n_total**: Total number of tealets created during the session

#### 2. Memory Allocation Tracking
All allocations through the tealet allocator interface are tracked incrementally:

- **bytes_allocated**: Current heap bytes allocated (all allocations, including tealets and stacks)
- **bytes_allocated_peak**: Maximum heap usage seen across the session
- **blocks_allocated**: Current number of allocated blocks
- **blocks_allocated_peak**: Maximum number of blocks allocated simultaneously
- **blocks_allocated_total**: Total allocation calls made

This tracks **all** memory allocated through the tealet allocator, including:
- Main tealet structure
- Regular tealet structures
- Stack chunk structures
- Stack segment data

#### 3. Stack-Specific Statistics
These are computed when `tealet_get_stats()` is called by walking all active tealets:

- **stack_count**: Number of distinct stack structures
- **stack_chunk_count**: Total number of stack chunks across all stacks
- **stack_bytes**: Actual bytes allocated for stack data (sum of all chunk sizes)
- **stack_bytes_expanded**: Logical stack bytes if all tealets had independent stacks
- **stack_bytes_naive**: Bytes that would be needed for fixed-size pre-allocated stacks

### Actual Memory Usage (Stack-Slicing)

Track all heap allocations made by libtealet:

- **bytes_allocated**: Tracked incrementally on every `malloc()`/`free()` call through the allocator
- **stack_bytes**: Computed by summing `chunk.size` across all stack chunks
- **stack_chunk_count**: Count of all stack segment allocations

This captures the real memory footprint of the stack-slicing approach.

### Stack Sharing Efficiency

When tealets are duplicated, they can share stack structures:

- **stack_count**: Number of unique stack structures
- **stack_chunk_count**: Total chunks across all stacks  
- **stack_bytes**: Actual bytes in stack data
- **stack_bytes_expanded**: What the bytes would be if stacks weren't shared

**Sharing ratio** = `stack_bytes_expanded / stack_bytes`

A ratio of 3.0x means three tealets are sharing the same stack data.

### Naive Memory Usage (Fixed-Size Stacks)

Calculate what a traditional fixed-size stack approach would require:

**Method: Sum Stack Extents**

For each tealet:
1. Find its first chunk (`stack->chunk`)
2. Calculate the full extent: `stack_far - chunk.stack_near`
3. Sum across all tealets

**Why this works:**
- `stack_far` is the highest stack address used by the tealet
- `chunk.stack_near` is the base of the stack
- The difference represents the maximum stack depth
- A naive implementation would pre-allocate this full range
- Stack-slicing only allocates the portions actually saved (`chunk.size`)

### Memory Efficiency Comparison

```
Actual memory (slicing)   = stack_bytes
Expanded memory (sharing) = stack_bytes_expanded  
Naive memory (fixed-size) = stack_bytes_naive
Sharing efficiency        = stack_bytes_expanded / stack_bytes
Memory efficiency         = stack_bytes_naive / stack_bytes
```

**Note:** Peak tracking happens on every allocation/deallocation (incremental), while naive and sharing metrics are computed on-demand when `tealet_get_stats()` is called (snapshot). This means `bytes_allocated_peak` might not correspond to the exact moment when `stack_bytes_naive` would also be at its peak.

## API

### Statistics Structure

```c
typedef struct tealet_stats_t {
    /* Basic counts */
    size_t n_active;                  /* Currently active tealets (including main) */
    size_t n_total;                   /* Total tealets created during session */
    
    /* Memory allocation tracking (incremental) */
    size_t bytes_allocated;           /* Current heap bytes allocated */
    size_t bytes_allocated_peak;      /* Peak heap allocation */
    size_t blocks_allocated;          /* Current number of blocks allocated */
    size_t blocks_allocated_peak;     /* Peak number of blocks */
    size_t blocks_allocated_total;    /* Total allocation calls made */
    
    /* Stack statistics (computed on-demand) */
    size_t stack_count;               /* Number of distinct stack structures */
    size_t stack_chunk_count;         /* Total stack chunks across all stacks */
    size_t stack_bytes;               /* Actual bytes in stack data */
    size_t stack_bytes_expanded;      /* Logical bytes if stacks not shared */
    size_t stack_bytes_naive;         /* Bytes for fixed-size pre-allocated stacks */
} tealet_stats_t;
```

**Note:** This structure is always available in the API, regardless of whether statistics are enabled. When `TEALET_WITH_STATS=0`, all fields will be zero.

### Functions

```c
/* Get current statistics */
void tealet_get_stats(tealet_t *main, tealet_stats_t *stats);

/* Reset peak counters (useful for benchmarking phases) */
void tealet_reset_peak_stats(tealet_t *main);
```

### Usage Example

```c
tealet_stats_t stats;
tealet_get_stats(main, &stats);

/* Check if stats are enabled */
if (stats.blocks_allocated > 0) {
    printf("Memory Statistics:\n");
    printf("  Active tealets: %zu (total created: %zu)\n", 
           stats.n_active, stats.n_total);
    printf("  Stack structures: %zu (%zu chunks)\n", 
           stats.stack_count, stats.stack_chunk_count);
    printf("\nMemory Usage:\n");
    printf("  Current allocated: %zu bytes (%zu blocks)\n",
           stats.bytes_allocated, stats.blocks_allocated);
    printf("  Peak allocated: %zu bytes (%zu blocks)\n",
           stats.bytes_allocated_peak, stats.blocks_allocated_peak);
    printf("  Total allocations: %zu\n", stats.blocks_allocated_total);
    printf("\nStack Memory:\n");
    printf("  Actual stack bytes: %zu\n", stats.stack_bytes);
    printf("  Expanded (if not shared): %zu\n", stats.stack_bytes_expanded);
    printf("  Naive (fixed-size): %zu\n", stats.stack_bytes_naive);
    
    if (stats.stack_bytes > 0) {
        printf("\nEfficiency:\n");
        if (stats.stack_bytes_expanded > stats.stack_bytes) {
            printf("  Sharing ratio: %.1fx\n", 
                   (double)stats.stack_bytes_expanded / stats.stack_bytes);
        }
        if (stats.stack_bytes_naive > stats.stack_bytes) {
            printf("  Memory savings vs naive: %.1fx\n",
                   (double)stats.stack_bytes_naive / stats.stack_bytes);
        } else {
            printf("  Memory overhead vs naive: %.1f%%\n",
                   100.0 * stats.stack_bytes / stats.stack_bytes_naive - 100.0);
        }
    }
} else {
    printf("Statistics not enabled (compiled with TEALET_WITH_STATS=0)\n");
}
```

## Implementation Details

### Tracking Allocations

All allocations through the tealet allocator are wrapped with statistics updates:

```c
#if TEALET_WITH_STATS
#define STATS_ADD_ALLOC(main, size) do { \
    main->g_stats.bytes_allocated += (size); \
    main->g_stats.blocks_allocated++; \
    main->g_stats.blocks_allocated_total++; \
    if (main->g_stats.bytes_allocated > main->g_stats.bytes_allocated_peak) \
        main->g_stats.bytes_allocated_peak = main->g_stats.bytes_allocated; \
    if (main->g_stats.blocks_allocated > main->g_stats.blocks_allocated_peak) \
        main->g_stats.blocks_allocated_peak = main->g_stats.blocks_allocated; \
} while(0)

#define STATS_SUB_ALLOC(main, size) do { \
    main->g_stats.bytes_allocated -= (size); \
    main->g_stats.blocks_allocated--; \
} while(0)
#else
#define STATS_ADD_ALLOC(main, size) ((void)0)
#define STATS_SUB_ALLOC(main, size) ((void)0)
#endif
```

All allocations are tracked, including:
- Tealet structures (main and regular)
- Stack structures
- Stack chunk metadata
- Stack segment data

Size information is stored in the allocated structures themselves, so deallocation tracking can subtract the correct amount.

### Computing Stack Statistics

The `tealet_get_stats()` function computes stack-related metrics by walking active tealets:

1. **Basic counts**: Maintained incrementally in the main tealet structure
2. **Memory tracking**: Updated on every allocation/deallocation
3. **Stack statistics**: Computed by traversing all active tealets and their stacks

For each tealet:
- Count unique stack structures (handle sharing via reference counting)
- Sum chunk counts across all stacks
- Sum `chunk.size` for actual bytes (stack_bytes)
- Sum stack extent for each tealet to get expanded bytes
- Sum `stack_far - chunk.stack_near` for naive fixed-size estimate

This approach handles:
- Stack sharing from `tealet_duplicate()`
- Multiple chunks per stack from stack growth
- Different stack depths per tealet

### Zero Overhead When Disabled

When `TEALET_WITH_STATS=0`:
- Statistics macros expand to `((void)0)` and are optimized away
- The `tealet_stats_t` structure remains in the API but all fields return zero
- No runtime cost whatsoever for allocation tracking
- `tealet_get_stats()` and `tealet_reset_peak_stats()` become no-ops

The statistics structure is always present in the API for ABI stability, but contains no data when disabled.

## Limitations

1. **Peak vs Snapshot Timing**: `bytes_allocated_peak` and `blocks_allocated_peak` are tracked incrementally on every allocation, while `stack_bytes_naive` is computed as a snapshot when `tealet_get_stats()` is called. The peak values might not correspond to the exact moment when naive usage would also be at its peak.

2. **Allocator Overhead**: Statistics track raw allocation sizes as requested, not including any metadata overhead added by the underlying allocator (malloc).

3. **Main Tealet Stack**: The main tealet uses the system stack (not heap-allocated), so it's not included in allocation tracking but is included in tealet counts.

4. **Incremental vs Computed**: Some metrics (bytes_allocated, blocks_allocated) are maintained incrementally for accuracy, while others (stack statistics) are computed on-demand to avoid overhead on every operation.

## Future Enhancements

- Track min/max/average stack depths
- Histogram of chunk sizes
- Per-tealet statistics (isolate memory usage per coroutine)
- Track stack growth events (how often stacks need to expand)
- Statistics export to JSON for external analysis
- Integration with memory profiling tools
- Track allocation/deallocation patterns and churn rate

## Test Programs

See the following test programs that demonstrate the statistics feature:

- **tests/test_stats.c**: Demonstrates basic statistics tracking, showing memory usage with tealets of varying stack depths
- **tests/test_sharing.c**: Shows how duplicated tealets share stack structures and the memory efficiency gains
- **tests/tests.c**: Main test suite with runtime statistics detection

Build and run:
```bash
make bin/test-stats bin/test-sharing
bin/test-stats
bin/test-sharing
```

## See Also

- [API Reference](API.md) - Full API documentation
- [Getting Started](GETTING_STARTED.md) - Basic usage examples
- **tests/test_stats.c** - Memory statistics demonstration
- **tests/test_sharing.c** - Stack sharing efficiency demonstration
- **tests/tests.c** - Main test suite with runtime stats detection
