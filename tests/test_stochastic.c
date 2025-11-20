/* Stochastic tealet switching test
 *
 * Tests realistic tealet usage patterns with:
 * - Multiple tealets created at varying stack depths
 * - Recursive worker function with stochastic decisions  
 * - Random switching between tealets
 * - Dynamic creation and cleanup
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "tealet.h"

/* Test parameters */
#define MAX_TEALETS 100
#define DEFAULT_TARGET_OPERATIONS 1000
#define DEFAULT_MAX_RECURSION_DEPTH 20
#define STATS_REPORT_INTERVAL 100

/* Global tealet registry */
static tealet_t *g_tealets[MAX_TEALETS];
static int g_tealet_count = 0;
static int g_next_id = 0;
static tealet_t *g_cleanup_slot = NULL;  /* Tealet waiting to be deleted */

/* Global counters */
static int g_total_operations = 0;
static int g_shutdown = 0;
static int g_clean_shutdown = 0;  /* Set via command line */
static int g_verbose = 0;  /* Set via command line */
static int g_target_operations = DEFAULT_TARGET_OPERATIONS;
static int g_max_recursion_depth = DEFAULT_MAX_RECURSION_DEPTH;

/* Main tealet */
static tealet_t *g_main = NULL;

/* Forward declaration */
static tealet_t *worker_entry(tealet_t *current, void *arg);

/* Add tealet to global registry */
static void add_tealet(tealet_t *t)
{
    if (g_tealet_count < MAX_TEALETS) {
        g_tealets[g_tealet_count++] = t;
    }
}

/* Remove tealet from global registry (swap with last element) */
static void remove_tealet(tealet_t *t)
{
    int i;
    for (i = 0; i < g_tealet_count; i++) {
        if (g_tealets[i] == t) {
            /* Swap with last element and decrement count */
            g_tealet_count--;
            g_tealets[i] = g_tealets[g_tealet_count];
            g_tealets[g_tealet_count] = NULL;
            return;
        }
    }
}

/* Pick a random tealet from registry (excluding current) */
static tealet_t *pick_random_tealet(tealet_t *exclude)
{
    int attempts = 10;
    while (attempts-- > 0 && g_tealet_count > 0) {
        int idx = rand() % g_tealet_count;
        if (g_tealets[idx] != exclude)
            return g_tealets[idx];
    }
    return NULL;
}

/* Print statistics */
static void print_stats(const char *label)
{
    tealet_stats_t stats;
    tealet_get_stats(g_main, &stats);
    
    printf("\n=== %s ===\n", label);
    printf("Operations:         %d\n", g_total_operations);
    printf("Tealets in list:    %d\n", g_tealet_count);
    printf("Active tealets:     %d\n", stats.n_active);
    printf("Stacks/chunks:      %zu / %zu\n", stats.stack_count, stats.stack_chunk_count);
    printf("Stack bytes:        %zu (expanded: %zu, naive: %zu)\n",
           stats.stack_bytes, stats.stack_bytes_expanded, stats.stack_bytes_naive);
    
    if (stats.stack_bytes > 0 && stats.stack_bytes_naive > 0) {
        double efficiency = 100.0 * stats.stack_bytes / stats.stack_bytes_naive;
        printf("Memory efficiency:  %.1f%% of naive\n", efficiency);
    }
    
    if (stats.stack_chunk_count > stats.stack_count) {
        double avg_chunks = (double)stats.stack_chunk_count / stats.stack_count;
        printf("Avg chunks/stack:   %.2f\n", avg_chunks);
    }
}

/* Main recursive worker function - makes stochastic decisions
 * Returns 0 normally, -1 to indicate voluntary exit */
static int worker_recursive(tealet_t *current, int depth)
{
    char buffer[256]; /* Some stack space */
    int choice;
    tealet_t *target;
    int i;
    
    /* Fill buffer to prevent optimization and consume stack */
    for (i = 0; i < 256; i++)
        buffer[i] = (char)(depth + i);
    
    /* Increment operations counter */
    g_total_operations++;
    
    /* Periodic stats (only in verbose mode) */
    if (g_verbose && g_total_operations % STATS_REPORT_INTERVAL == 0) {
        print_stats("Progress");
    }
    
    /* Check shutdown condition */
    if (g_total_operations >= g_target_operations) {
        if (!g_shutdown) {
            if (g_verbose) {
                printf("\nTarget reached at depth %d! Shutting down... (ops=%d, target=%d)\n", 
                       depth, g_total_operations, g_target_operations);
                printf("Active tealets before shutdown: %d\n", g_tealet_count);
                printf("Current tealet: %s\n", current == g_main ? "MAIN" : "CHILD");
            }
            g_shutdown = 1;
        }
    }
    
    /* Make stochastic decision - loop until we find a valid choice */
    while (1) {
        /* Check if there's a tealet in cleanup slot and delete it */
        if (g_cleanup_slot) {
            if (g_verbose) {
                printf("Tealet at depth %d cleaning up exited tealet\n", depth);
            }
            tealet_delete(g_cleanup_slot);
            g_cleanup_slot = NULL;
        }
        
        /* Check shutdown at the start of each iteration */
        if (g_shutdown) {
            if (g_clean_shutdown) {
                /* Clean shutdown: just return to unwind recursion */
                return 0;
            } else {
                /* Immediate exit: child tealets switch to main, main unwinds */
                if (current != g_main) {
                    tealet_switch(g_main, NULL);
                    /* we should not resume this */
                    assert(0);
                }
                return 0;
            }
        }
        
        choice = rand() % 5;
        
        if (choice == 0 && depth > 0) {
            /* Return from recursion (but not from depth 0) */
            return 0;
            
        } else if (choice == 1 && depth < g_max_recursion_depth) {
            /* Recurse deeper */
            int result = worker_recursive(current, depth + 1);
            if (result < 0) {
                /* Propagate exit signal */
                return result;
            }
            continue;
            
        } else if (choice == 2 && g_tealet_count > 1) {
            /* Switch to another tealet */
            target = pick_random_tealet(current);
            if (target) {
                tealet_switch(target, NULL);
            }
            continue;
            
        } else if (choice == 3  && g_tealet_count < MAX_TEALETS) {
            /* Create new tealet at current stack depth */
            void *arg = NULL;
            tealet_new(current, worker_entry, &arg);
            continue;
            
        } else if (choice == 4 && current != g_main && g_tealet_count >= MAX_TEALETS) {
            /* Exit this tealet (only when pool is full and we're not main) */
            if (g_clean_shutdown) {
                /* Clean mode: return -1 to unwind and exit cleanly */
                if (g_verbose) {
                    printf("Tealet at depth %d requesting exit (pool full)\\n", depth);
                }
                return -1;
            } else {
                /* Non-clean mode: exit directly by switching to another tealet */
                if (g_verbose) {
                    printf("Tealet at depth %d exiting voluntarily (pool full)\\n", depth);
                }
                /* Remove ourselves from the list and place in cleanup slot */
                remove_tealet(current);
                g_cleanup_slot = current;
                /* Switch to a random tealet */
                target = pick_random_tealet(current);
                if (target) {
                    tealet_switch(target, NULL);
                }
                /* We shouldn't resume here - we've exited */
                assert(0);
            }
        }
        /* If choice was invalid, loop and try again */
    }
    
    /* Prevent buffer optimization */
    if (buffer[0] == 0)
        (void)fputc('\0', stdout);
    
    return 0;
}

/* Tealet entry point */
static tealet_t *worker_entry(tealet_t *current, void *arg)
{
    int my_id;
    int result;
    (void)arg;
    
    /* Add ourselves to the registry */
    add_tealet(current);
    
    /* Assign ID when tealet starts running */
    my_id = g_next_id++;
    if (g_verbose) {
        printf("Created tealet %d (total: %d)\n", my_id, g_tealet_count);
    }
    
    /* Start recursive worker at depth 0 */
    result = worker_recursive(current, 0);
    
    /* Handle result */
    if (result < 0) {
        /* Voluntary exit requested (clean mode only) */
        tealet_t *target;
        if (g_verbose) {
            printf("Tealet %d exiting cleanly after unwinding\n", my_id);
        }
        /* Remove ourselves from the list and place in cleanup slot */
        remove_tealet(current);
        g_cleanup_slot = current;
        /* Exit with defer flag to return cleanly, then switch to random tealet */
        target = pick_random_tealet(current);
        if (target) {
            tealet_exit(target, NULL, TEALET_FLAG_DEFER);
        } else {
            tealet_exit(g_main, NULL, TEALET_FLAG_DEFER);
        }
        /* Return cleanly - tealet_exit with DEFER will handle the switch */
        return g_main;
    }
    
    /* When we return here normally, we're done - exit to main without auto-delete */
    tealet_exit(g_main, NULL, TEALET_FLAG_NONE);
    
    /* Unreachable */
    return g_main;
}

int main(int argc, char *argv[])
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    int i;
    
    /* Parse command line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clean") == 0) {
            g_clean_shutdown = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--operations") == 0) {
            if (i + 1 < argc) {
                g_target_operations = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--depth") == 0) {
            if (i + 1 < argc) {
                g_max_recursion_depth = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -c, --clean              Clean shutdown (unwind stacks)\n");
            printf("  -v, --verbose            Verbose output (progress stats)\n");
            printf("  -n, --operations <num>   Target operations (default: %d)\n", DEFAULT_TARGET_OPERATIONS);
            printf("  -d, --depth <num>        Max recursion depth (default: %d)\n", DEFAULT_MAX_RECURSION_DEPTH);
            printf("  -h, --help               Show this help\n");
            return 0;
        }
    }
    
    printf("Stochastic Tealet Test\n");
    printf("======================\n");
    printf("Target operations: %d\n", g_target_operations);
    printf("Max recursion depth: %d\n", g_max_recursion_depth);
    printf("Max tealets: %d\n", MAX_TEALETS);
    printf("Shutdown mode: %s\n\n", g_clean_shutdown ? "CLEAN (unwind stacks)" : "IMMEDIATE (delete active)");
    
    srand(42); /* Deterministic for reproducibility */
    
    g_main = tealet_initialize(&alloc, 0);
    if (!g_main) {
        fprintf(stderr, "Failed to initialize\n");
        return 1;
    }
    
    /* Add main tealet to the registry */
    add_tealet(g_main);
    
    print_stats("Initial");
    
    /* Main tealet participates in the recursive work */
    printf("\nMain tealet starting recursive work...\n");
    worker_recursive(g_main, 0);
    
    /* When we return, shutdown was triggered */
    printf("\nShutdown triggered, cleaning up...\n");
    print_stats("After shutdown");
    
    if (g_clean_shutdown) {
        /* Clean shutdown: switch to each tealet to let them unwind their stacks */
        if (g_verbose) {
            printf("\nLetting tealets unwind their stacks...\n");
        }
        for (i = 0; i < g_tealet_count; i++) {
            if (g_tealets[i] && g_tealets[i] != g_main) {
                int status = tealet_status(g_tealets[i]);
                if (status == TEALET_STATUS_ACTIVE) {
                    if (g_verbose) {
                        printf("  Switching to tealet %d to unwind\n", i);
                    }
                    tealet_switch(g_tealets[i], NULL);
                }
            }
        }
        if (g_verbose) {
            printf("All tealets unwound.\n");
        }
    }
    
    /* Delete all remaining tealets (except main) */
    if (g_verbose) {
        printf("\nDeleting %d tealets...\n", g_tealet_count);
    }
    for (i = 0; i < g_tealet_count; i++) {
        if (g_tealets[i] && g_tealets[i] != g_main) {
            if (g_verbose) {
                int status = tealet_status(g_tealets[i]);
                printf("  Tealet %d: status=%d (%s)\n", i, status,
                       status == TEALET_STATUS_ACTIVE ? "ACTIVE" :
                       status == TEALET_STATUS_EXITED ? "EXITED" :
                       status == TEALET_STATUS_DEFUNCT ? "DEFUNCT" : "UNKNOWN");
            }
            tealet_delete(g_tealets[i]);
        }
    }
    
    print_stats("Final");
    
    tealet_finalize(g_main);
    
    printf("\n✓ Test completed\n");
    printf("Total operations: %d\n", g_total_operations);
    printf("Tealets created: %d\n", g_next_id);
    
    return 0;
}
