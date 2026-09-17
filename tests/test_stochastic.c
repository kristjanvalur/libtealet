/* Stochastic tealet switching test
 *
 * Tests realistic tealet usage patterns with:
 * - Multiple tealets created at varying stack depths
 * - Recursive worker function with stochastic decisions
 * - Random switching between tealets
 * - Dynamic creation and cleanup
 *
 * Also compares in-place tealet_new()/tealet_run() vs shared-stub creation.
 * In-place tealets are based at the caller's stack depth.  Stub-based
 * tealets are duplicated from one template, so they share a single
 * stack base and overlap to different degrees.
 *
 *   bin/test-stochastic --compare -n 20000
 *   bin/test-stochastic --compare -n 20000 -s 20
 *   make bench-stubs
 */
#include "tealet.h"
#include "tealet_extras.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* Test parameters */
#define MAX_TEALETS 100
#define DEFAULT_TARGET_OPERATIONS 1000
#define DEFAULT_MAX_RECURSION_DEPTH 20
#define STATS_REPORT_INTERVAL 100
#define PEAK_SAMPLE_INTERVAL 10

typedef enum { MODE_INPLACE = 0, MODE_STUB = 1 } create_mode_t;

typedef struct {
  const char *name;
  create_mode_t mode;
  int stub_depth;
  int operations;
  int creates;
  int switches;
  int exits;
  int tealets_created;
  double time_ms;
  size_t peak_bytes_allocated;
  size_t peak_stack_bytes;
  size_t peak_stack_naive;
  size_t peak_stack_expanded;
  size_t peak_stack_count;
  size_t peak_stack_chunks;
  int peak_n_active;
  int peak_unique_fars;
  size_t blocks_allocated_total;
} trial_result_t;

/* Global tealet registry */
static tealet_t *g_tealets[MAX_TEALETS];
static int g_tealet_count = 0;
static int g_next_id = 0;
static tealet_t *g_cleanup_slot = NULL; /* Tealet waiting to be deleted */

/* Global counters */
static int g_total_operations = 0;
static int g_shutdown = 0;
static int g_clean_shutdown = 0; /* Set via command line */
static int g_verbose = 0;        /* Set via command line */
static int g_quiet = 0;          /* Suppress per-trial banners (compare mode) */
static int g_target_operations = DEFAULT_TARGET_OPERATIONS;
static int g_max_recursion_depth = DEFAULT_MAX_RECURSION_DEPTH;
static int g_seed = 42;
static int g_stub_depth = 0;
static create_mode_t g_create_mode = MODE_INPLACE;
static int g_creates = 0;
static int g_switches = 0;
static int g_exits = 0;

/* Main tealet and optional shared stub template */
static tealet_t *g_main = NULL;
static tealet_t *g_stub = NULL;

/* Peak tracking for the current trial */
static trial_result_t g_trial;
static char g_stub_name[32];

/* Forward declaration */
static tealet_t *worker_entry(tealet_t *current, void *arg);

static double now_ms(void) {
#ifdef _WIN32
  static LARGE_INTEGER freq;
  LARGE_INTEGER t;
  if (freq.QuadPart == 0)
    QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

/* Add tealet to global registry */
static void add_tealet(tealet_t *t) {
  if (g_tealet_count < MAX_TEALETS) {
    g_tealets[g_tealet_count++] = t;
  }
}

/* Remove tealet from global registry (swap with last element) */
static void remove_tealet(tealet_t *t) {
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
static tealet_t *pick_random_tealet(tealet_t *exclude) {
  int attempts = 10;
  while (attempts-- > 0 && g_tealet_count > 0) {
    int idx = rand() % g_tealet_count;
    if (g_tealets[idx] != exclude)
      return g_tealets[idx];
  }
  return NULL;
}

/* Distinct stack_far values among registered child tealets.
 * In-place creation bases each tealet at the caller's depth.
 * Stub creation duplicates one template, so children share one base.
 */
static int count_unique_child_fars(void) {
  void *fars[MAX_TEALETS];
  int n = 0;
  int i, j;

  for (i = 0; i < g_tealet_count; i++) {
    void *far;
    int seen = 0;
    if (g_tealets[i] == NULL || g_tealets[i] == g_main)
      continue;
    far = tealet_get_far(g_tealets[i]);
    for (j = 0; j < n; j++) {
      if (fars[j] == far) {
        seen = 1;
        break;
      }
    }
    if (!seen && n < MAX_TEALETS)
      fars[n++] = far;
  }
  return n;
}

static void sample_peaks(void) {
  tealet_stats_t stats;
  int unique_fars;

  tealet_get_stats(g_main, &stats);
  unique_fars = count_unique_child_fars();

  if (stats.bytes_allocated_peak > g_trial.peak_bytes_allocated)
    g_trial.peak_bytes_allocated = stats.bytes_allocated_peak;
  if (stats.n_active > g_trial.peak_n_active)
    g_trial.peak_n_active = stats.n_active;
  if (unique_fars > g_trial.peak_unique_fars)
    g_trial.peak_unique_fars = unique_fars;

  if (stats.stack_bytes >= g_trial.peak_stack_bytes) {
    g_trial.peak_stack_bytes = stats.stack_bytes;
    g_trial.peak_stack_naive = stats.stack_bytes_naive;
    g_trial.peak_stack_expanded = stats.stack_bytes_expanded;
    g_trial.peak_stack_count = stats.stack_count;
    g_trial.peak_stack_chunks = stats.stack_chunk_count;
  }
}

/* Print statistics */
static void print_stats(const char *label) {
  tealet_stats_t stats;
  tealet_get_stats(g_main, &stats);

  printf("\n=== %s ===\n", label);
  printf("Operations:         %d\n", g_total_operations);
  printf("Tealets in list:    %d\n", g_tealet_count);
  printf("Active tealets:     %d\n", stats.n_active);
  printf("Unique child bases: %d\n", count_unique_child_fars());
  printf("Stacks/chunks:      %zu / %zu\n", stats.stack_count, stats.stack_chunk_count);
  printf("Stack bytes:        %zu (expanded: %zu, naive: %zu)\n", stats.stack_bytes, stats.stack_bytes_expanded,
         stats.stack_bytes_naive);

  if (stats.stack_bytes > 0 && stats.stack_bytes_naive > 0) {
    double efficiency = 100.0 * stats.stack_bytes / stats.stack_bytes_naive;
    printf("Memory efficiency:  %.1f%% of naive\n", efficiency);
  }

  if (stats.stack_chunk_count > stats.stack_count) {
    double avg_chunks = (double)stats.stack_chunk_count / stats.stack_count;
    printf("Avg chunks/stack:   %.2f\n", avg_chunks);
  }
}

static int do_switch(tealet_t *target) {
  g_switches++;
  return tealet_switch(target, NULL, TEALET_XFER_DEFAULT);
}

static int do_exit(tealet_t *target, void *arg, int flags) {
  g_exits++;
  return tealet_exit(target, arg, flags);
}

/* Create a worker either in place at the current stack depth, or by
 * duplicating the shared stub (same stack base for every child).
 */
static void do_create(tealet_t *current) {
  void *arg = NULL;

  g_creates++;
  if (g_create_mode == MODE_STUB) {
    tealet_t *dup;
    int res;
    assert(g_stub != NULL);
    dup = tealet_duplicate(g_stub);
    if (dup == NULL)
      return;
    res = tealet_stub_run(dup, worker_entry, &arg);
    if (res)
      tealet_delete(dup);
    return;
  }
  {
    tealet_t *created = tealet_new(current);
    if (created != NULL) {
      if (tealet_run(created, worker_entry, &arg, NULL, TEALET_START_SWITCH) != 0)
        tealet_delete(created);
    }
  }
}

/* Recurse then snapshot a stub so all later duplicates share this base.
 * The buffer is used after the recursive call so the compiler cannot
 * tail-call and collapse the intended stack depth.
 */
static tealet_t *make_stub_at_depth(tealet_t *t, int depth) {
  volatile char buffer[256];
  tealet_t *stub = NULL;
  int i;

  for (i = 0; i < 256; i++)
    buffer[i] = (char)(depth + i);
  if (depth > 0)
    stub = make_stub_at_depth(t, depth - 1);
  else if (tealet_stub_new(t, &stub, NULL) != 0)
    stub = NULL;
  if (buffer[0] == 0)
    (void)fputc('\0', stdout);
  return stub;
}

/* Main recursive worker function - makes stochastic decisions
 * Returns 0 normally, -1 to indicate voluntary exit */
static int worker_recursive(tealet_t *current, int depth) {
  char buffer[256]; /* Some stack space */
  int choice;
  tealet_t *target;
  int i;

  /* Fill buffer to prevent optimization and consume stack */
  for (i = 0; i < 256; i++)
    buffer[i] = (char)(depth + i);

  /* Increment operations counter */
  g_total_operations++;

  if (g_total_operations % PEAK_SAMPLE_INTERVAL == 0)
    sample_peaks();

  /* Periodic stats (only in verbose mode) */
  if (g_verbose && g_total_operations % STATS_REPORT_INTERVAL == 0) {
    print_stats("Progress");
  }

  /* Check shutdown condition */
  if (g_total_operations >= g_target_operations) {
    if (!g_shutdown) {
      if (g_verbose) {
        printf("\nTarget reached at depth %d! Shutting down... (ops=%d, "
               "target=%d)\n",
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
          do_switch(g_main);
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
        do_switch(target);
      }
      continue;

    } else if (choice == 3 && g_tealet_count < MAX_TEALETS) {
      /* Create a new tealet: in-place at this depth, or from the stub */
      do_create(current);
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
          do_switch(target);
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
static tealet_t *worker_entry(tealet_t *current, void *arg) {
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
      do_exit(target, NULL, TEALET_EXIT_DEFER);
    } else {
      do_exit(g_main, NULL, TEALET_EXIT_DEFER);
    }
    /* Return cleanly - tealet_exit with DEFER will handle the switch */
    return g_main;
  }

  /* When we return here normally, we're done - exit to main without auto-delete
   */
  do_exit(g_main, NULL, TEALET_XFER_DEFAULT);

  /* Unreachable */
  return g_main;
}

static void reset_trial_state(void) {
  int i;
  for (i = 0; i < MAX_TEALETS; i++)
    g_tealets[i] = NULL;
  g_tealet_count = 0;
  g_next_id = 0;
  g_cleanup_slot = NULL;
  g_total_operations = 0;
  g_shutdown = 0;
  g_main = NULL;
  g_stub = NULL;
  g_creates = 0;
  g_switches = 0;
  g_exits = 0;
  memset(&g_trial, 0, sizeof(g_trial));
}

static int run_trial(create_mode_t mode, int stub_depth, trial_result_t *out) {
  tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
  tealet_stats_t stats;
  double t0, t1;
  int configure_result;
  int i;

  reset_trial_state();
  g_create_mode = mode;
  g_trial.mode = mode;
  g_trial.stub_depth = stub_depth;
  if (mode == MODE_STUB) {
    snprintf(g_stub_name, sizeof(g_stub_name), "stub@%d", stub_depth);
    g_trial.name = g_stub_name;
  } else {
    g_trial.name = "inplace";
  }

  srand(g_seed);

  g_main = tealet_initialize(&alloc, 0);
  if (!g_main) {
    fprintf(stderr, "Failed to initialize\n");
    return 1;
  }
  configure_result = tealet_configure_check_stack(g_main, 0);
  if (configure_result != 0) {
    fprintf(stderr, "Failed to enable stack checks: %d\n", configure_result);
    tealet_finalize(g_main);
    g_main = NULL;
    return 1;
  }

  if (mode == MODE_STUB) {
    g_stub = make_stub_at_depth(g_main, stub_depth);
    if (!g_stub) {
      fprintf(stderr, "Failed to create stub at depth %d\n", stub_depth);
      tealet_finalize(g_main);
      g_main = NULL;
      return 1;
    }
  }

  /* Add main tealet to the registry */
  add_tealet(g_main);

  if (!g_quiet) {
    print_stats("Initial");
    printf("\nMain tealet starting recursive work (%s", g_trial.name);
    if (mode == MODE_STUB)
      printf(", stub-depth %d", stub_depth);
    printf(")...\n");
  }

  t0 = now_ms();
  worker_recursive(g_main, 0);
  t1 = now_ms();
  sample_peaks();

  if (!g_quiet) {
    printf("\nShutdown triggered, cleaning up...\n");
    print_stats("After shutdown");
  }

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
          do_switch(g_tealets[i]);
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
               status == TEALET_STATUS_ACTIVE    ? "ACTIVE"
               : status == TEALET_STATUS_NEW     ? "NEW"
               : status == TEALET_STATUS_EXITED  ? "EXITED"
               : status == TEALET_STATUS_DEFUNCT ? "DEFUNCT"
                                                 : "UNKNOWN");
      }
      tealet_delete(g_tealets[i]);
      g_tealets[i] = NULL;
    }
  }

  if (g_stub) {
    tealet_delete(g_stub);
    g_stub = NULL;
  }

  tealet_get_stats(g_main, &stats);
  if (!g_quiet)
    print_stats("Final");

  g_trial.operations = g_total_operations;
  g_trial.creates = g_creates;
  g_trial.switches = g_switches;
  g_trial.exits = g_exits;
  g_trial.tealets_created = g_next_id;
  g_trial.time_ms = t1 - t0;
  if (stats.bytes_allocated_peak > g_trial.peak_bytes_allocated)
    g_trial.peak_bytes_allocated = stats.bytes_allocated_peak;
  g_trial.blocks_allocated_total = stats.blocks_allocated_total;

  if (!g_quiet) {
    printf("\n%s completed in %.3f ms\n", g_trial.name, g_trial.time_ms);
    printf("Total operations: %d\n", g_total_operations);
    printf("Creates/switches/exits: %d / %d / %d\n", g_creates, g_switches, g_exits);
    printf("Tealets created: %d\n", g_next_id);
    printf("Peak heap: %zu bytes, peak stack: %zu (naive %zu, expanded %zu)\n", g_trial.peak_bytes_allocated,
           g_trial.peak_stack_bytes, g_trial.peak_stack_naive, g_trial.peak_stack_expanded);
    printf("Peak unique child stack bases: %d\n", g_trial.peak_unique_fars);
  }

  tealet_finalize(g_main);
  g_main = NULL;

  if (out)
    *out = g_trial;
  return 0;
}

static void print_ratio_header(const char *left, const char *right) {
  printf("%-28s %14s %14s %16s\n", "Metric", left, right, "ratio");
  printf("%-28s %14s %14s %16s\n", "----------------------------", "--------------", "--------------",
         "----------------");
}

static void print_ratio_int(const char *name, int a, int b) {
  if (a == 0)
    printf("  %-26s %14d %14d %16s\n", name, a, b, "n/a");
  else
    printf("  %-26s %14d %14d %16.3f\n", name, a, b, (double)b / (double)a);
}

static void print_ratio_size(const char *name, size_t a, size_t b) {
  if (a == 0)
    printf("  %-26s %14zu %14zu %16s\n", name, a, b, "n/a");
  else
    printf("  %-26s %14zu %14zu %16.3f\n", name, a, b, (double)b / (double)a);
}

static void print_ratio_dbl(const char *name, double a, double b) {
  if (a == 0.0)
    printf("  %-26s %14.3f %14.3f %16s\n", name, a, b, "n/a");
  else
    printf("  %-26s %14.3f %14.3f %16.3f\n", name, a, b, b / a);
}

static void print_comparison(const trial_result_t *a, const trial_result_t *b) {
  double a_share = 0.0, b_share = 0.0;
  double a_vs_naive = 0.0, b_vs_naive = 0.0;

  if (a->peak_stack_bytes > 0)
    a_share = (double)a->peak_stack_expanded / (double)a->peak_stack_bytes;
  if (b->peak_stack_bytes > 0)
    b_share = (double)b->peak_stack_expanded / (double)b->peak_stack_bytes;
  if (a->peak_stack_naive > 0)
    a_vs_naive = 100.0 * (double)a->peak_stack_bytes / (double)a->peak_stack_naive;
  if (b->peak_stack_naive > 0)
    b_vs_naive = 100.0 * (double)b->peak_stack_bytes / (double)b->peak_stack_naive;

  printf("\n");
  printf("================================================================\n");
  printf("inplace vs stub  (seed=%d, ops=%d, depth=%d, stub-depth=%d, max tealets=%d)\n", g_seed, g_target_operations,
         g_max_recursion_depth, b->stub_depth, MAX_TEALETS);
  printf("================================================================\n");
  print_ratio_header(a->name, b->name);
  print_ratio_dbl("Time (ms)", a->time_ms, b->time_ms);
  print_ratio_int("Operations", a->operations, b->operations);
  print_ratio_int("Creates", a->creates, b->creates);
  print_ratio_int("Switches", a->switches, b->switches);
  print_ratio_int("Exits", a->exits, b->exits);
  print_ratio_int("Tealets created", a->tealets_created, b->tealets_created);
  print_ratio_int("Peak unique child bases", a->peak_unique_fars, b->peak_unique_fars);
  print_ratio_int("Peak active tealets", a->peak_n_active, b->peak_n_active);
  print_ratio_size("Peak heap (bytes)", a->peak_bytes_allocated, b->peak_bytes_allocated);
  print_ratio_size("Peak stack (bytes)", a->peak_stack_bytes, b->peak_stack_bytes);
  print_ratio_size("Peak naive (bytes)", a->peak_stack_naive, b->peak_stack_naive);
  print_ratio_size("Peak expanded (bytes)", a->peak_stack_expanded, b->peak_stack_expanded);
  print_ratio_size("Peak stacks", a->peak_stack_count, b->peak_stack_count);
  print_ratio_size("Peak chunks", a->peak_stack_chunks, b->peak_stack_chunks);
  print_ratio_dbl("Sharing at stack peak", a_share, b_share);
  print_ratio_dbl("Stack vs naive at peak %", a_vs_naive, b_vs_naive);
  print_ratio_size("Total alloc calls", a->blocks_allocated_total, b->blocks_allocated_total);
  printf("\n");
  printf("Notes:\n");
  printf("  ratio > 1 means stub is larger/slower than in-place.\n");
  printf("  In-place tealets are based at the caller's stack depth.\n");
  printf("  Stub tealets duplicate one template, so they share a stack base\n");
  printf("  and overlap to different degrees as they recurse.\n");
  printf("  Switch counts exclude the switch inside tealet_run()/stub_run().\n");
}

static void print_usage(const char *argv0) {
  printf("Usage: %s [options]\n", argv0);
  printf("Options:\n");
  printf("  -c, --clean              Clean shutdown (unwind stacks)\n");
  printf("  -v, --verbose            Verbose output (progress stats)\n");
  printf("  -n, --operations <num>   Target operations (default: %d)\n", DEFAULT_TARGET_OPERATIONS);
  printf("  -d, --depth <num>        Max recursion depth (default: %d)\n", DEFAULT_MAX_RECURSION_DEPTH);
  printf("  -m, --mode <mode>        Creation mode: inplace, stub, compare (default: inplace)\n");
  printf("      --compare            Shorthand for --mode compare\n");
  printf("  -s, --stub-depth <num>   Recursion depth of the shared stub (default: 0)\n");
  printf("      --seed <num>         RNG seed (default: 42)\n");
  printf("  -h, --help               Show this help\n");
}

int main(int argc, char *argv[]) {
  int i;
  int compare = 0;
  create_mode_t mode = MODE_INPLACE;
  trial_result_t inplace_res, stub_res;

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
    } else if (strcmp(argv[i], "--compare") == 0) {
      compare = 1;
    } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mode") == 0) {
      if (i + 1 < argc) {
        const char *m = argv[++i];
        if (strcmp(m, "inplace") == 0) {
          mode = MODE_INPLACE;
        } else if (strcmp(m, "stub") == 0) {
          mode = MODE_STUB;
        } else if (strcmp(m, "compare") == 0) {
          compare = 1;
        } else {
          fprintf(stderr, "Unknown mode '%s' (use inplace, stub, or compare)\n", m);
          return 1;
        }
      }
    } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stub-depth") == 0) {
      if (i + 1 < argc) {
        g_stub_depth = atoi(argv[++i]);
      }
    } else if (strcmp(argv[i], "--seed") == 0) {
      if (i + 1 < argc) {
        g_seed = atoi(argv[++i]);
      }
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  printf("Stochastic Tealet Test\n");
  printf("======================\n");
  printf("Target operations: %d\n", g_target_operations);
  printf("Max recursion depth: %d\n", g_max_recursion_depth);
  printf("Max tealets: %d\n", MAX_TEALETS);
  printf("Seed: %d\n", g_seed);
  printf("Shutdown mode: %s\n", g_clean_shutdown ? "CLEAN (unwind stacks)" : "IMMEDIATE (delete active)");
  if (compare)
    printf("Creation mode: compare (inplace vs stub at depth %d)\n\n", g_stub_depth);
  else if (mode == MODE_STUB)
    printf("Creation mode: stub (shared base at depth %d)\n\n", g_stub_depth);
  else
    printf("Creation mode: inplace (base at caller depth)\n\n");

  if (compare) {
    int stub_depth = g_stub_depth;
    g_quiet = !g_verbose;
    if (run_trial(MODE_INPLACE, 0, &inplace_res))
      return 1;
    if (run_trial(MODE_STUB, stub_depth, &stub_res))
      return 1;
    print_comparison(&inplace_res, &stub_res);
    return 0;
  }

  if (run_trial(mode, g_stub_depth, NULL))
    return 1;

  printf("\n✓ Test completed\n");
  return 0;
}
