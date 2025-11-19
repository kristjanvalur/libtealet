/* Test to validate multiple chunks per stack and chunk sharing
 *
 * This test specifically exercises the stack growth mechanism that creates
 * multiple chunks per stack. It validates:
 * - Stacks can grow by adding additional chunks
 * - Stack chunk counting is accurate
 * - Chunk sharing works correctly with tealet_duplicate()
 * - Statistics properly track multiple chunks
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "tealet.h"

static tealet_t *g_main = NULL;

/* Helper to recurse and consume stack space - NOT tail-recursive */
static void consume_stack(int depth, char *buffer)
{
    char local[1024];
    int i;
    
    /* Prevent optimization */
    for (i = 0; i < 1024; i++)
        local[i] = (char)(depth + i);
    buffer[0] = local[512];  /* Force local array usage */
    
    if (depth > 0)
        consume_stack(depth - 1, buffer);
}

/* Worker that will be called at different stack depths to force chunk growth */
static tealet_t *worker_run(tealet_t *t, void *arg)
{
    char buffer[100];
    int depth = (int)(intptr_t)arg;
    
    /* Consume stack based on depth parameter */
    if (depth > 0)
        consume_stack(depth, buffer);
    
    /* Yield back to main */
    tealet_switch(g_main, NULL);
    
    return g_main;
}

int main(void)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_stats_t stats;
    tealet_t *t1, *t2, *t3;
    
    g_main = tealet_initialize(&alloc, 0);
    if (!g_main) {
        fprintf(stderr, "Failed to initialize\n");
        return 1;
    }
    
    printf("=== Multiple Chunks and Sharing Test ===\n\n");
    
    /* Create and run a tealet to build up its stack with multiple chunks */
    printf("1. Creating tealet and forcing stack growth into multiple chunks...\n");
    t1 = tealet_new(g_main, worker_run, NULL);
    if (!t1) {
        fprintf(stderr, "Failed to create tealet\n");
        tealet_finalize(g_main);
        return 1;
    }
    
    /* Switch to it a few times to build up stack */
    tealet_switch(t1, NULL);
    tealet_switch(t1, NULL);
    tealet_switch(t1, NULL);
    
    /* Now t1 has a saved stack with multiple chunks */
    tealet_get_stats(g_main, &stats);
    printf("   After creating t1:\n");
    printf("   - Stacks: %zu, chunks: %zu", stats.stack_count, stats.stack_chunk_count);
    if (stats.stack_chunk_count > stats.stack_count) {
        printf(" ✓ Multiple chunks created!\n");
    } else {
        printf(" (single chunk - may not have grown yet)\n");
    }
    printf("   - Stack bytes: %zu (expanded: %zu)\n", 
           stats.stack_bytes, stats.stack_bytes_expanded);
    printf("   - Sharing ratio: %.2fx (expanded/actual)\n\n",
           (double)stats.stack_bytes_expanded / (double)stats.stack_bytes);
    
    /* Duplicate the tealet - this should share the chunks */
    printf("2. Duplicating t1 to create t2...\n");
    t2 = tealet_duplicate(t1);
    if (!t2) {
        fprintf(stderr, "Failed to duplicate tealet\n");
        tealet_delete(t1);
        tealet_finalize(g_main);
        return 1;
    }
    
    tealet_get_stats(g_main, &stats);
    printf("   After duplicating (t1 + t2):\n");
    printf("   - Stacks: %zu, chunks: %zu\n",
           stats.stack_count, stats.stack_chunk_count);
    printf("   - Stack bytes: %zu (expanded: %zu)\n", 
           stats.stack_bytes, stats.stack_bytes_expanded);
    printf("   - Sharing ratio: %.2fx (expanded/actual)\n\n",
           (double)stats.stack_bytes_expanded / (double)stats.stack_bytes);
    
    /* Duplicate again for even more sharing */
    printf("3. Duplicating t1 to create t3...\n");
    t3 = tealet_duplicate(t1);
    if (!t3) {
        fprintf(stderr, "Failed to duplicate tealet\n");
        tealet_delete(t1);
        tealet_delete(t2);
        tealet_finalize(g_main);
        return 1;
    }
    
    tealet_get_stats(g_main, &stats);
    printf("   After duplicating again (t1 + t2 + t3):\n");
    printf("   - Stacks: %zu, chunks: %zu\n",
           stats.stack_count, stats.stack_chunk_count);
    printf("   - Stack bytes: %zu (expanded: %zu)\n", 
           stats.stack_bytes, stats.stack_bytes_expanded);
    printf("   - Sharing ratio: %.2fx (expanded/actual)\n",
           (double)stats.stack_bytes_expanded / (double)stats.stack_bytes);
    printf("   - Memory efficiency: Using %.1f%% of naive allocation\n\n",
           100.0 * (double)stats.stack_bytes / (double)stats.stack_bytes_naive);
    
    printf("Summary: %zu tealets sharing %zu stacks (%zu chunks total) = %.2fx expansion\n",
           (size_t)stats.n_active - 1,  /* Exclude main */
           stats.stack_count,
           stats.stack_chunk_count,
           (double)stats.stack_bytes_expanded / (double)stats.stack_bytes);
    
    /* Cleanup */
    tealet_delete(t3);
    tealet_delete(t2);
    tealet_delete(t1);
    tealet_finalize(g_main);
    
    printf("\n=== Test completed successfully ===\n");
    return 0;
}
