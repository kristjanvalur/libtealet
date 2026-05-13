#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tealet.h"
#include "tealet_extras.h"
#include "test_harness.h"
#include "test_lifecycle.h"
#include "test_lock_helpers.h"
#include "test_locking.h"
#include "test_resilience.h"
#include "test_stack.h"
#include "test_stats_extra.h"
#include "test_stress.h"
#include "test_transfer.h"

int status = 0;
tealet_t *g_main = NULL;
static tealet_t *the_stub = NULL;
static int newmode = 0;
tealet_test_lock_state_t g_lock_state;
static int g_locking_enabled = 0;

/* Runtime detection of stats support */
static int g_stats_enabled = 0;
static int stats_check_count = 0;
static size_t stats_max_allocated = 0;
static size_t stats_max_naive = 0;

/* Check stats periodically during tests */
void check_stats(int verbose) {
  tealet_stats_t stats;

  if (!g_stats_enabled)
    return;

  stats_check_count++;

  tealet_get_stats(g_main, &stats);

  /* Track maximums */
  if (stats.bytes_allocated > stats_max_allocated)
    stats_max_allocated = stats.bytes_allocated;
  if (stats.stack_bytes_naive > stats_max_naive)
    stats_max_naive = stats.stack_bytes_naive;

  if (verbose) {
    printf("Stats check #%d: %d active, %zu bytes allocated, %zu naive, %zu "
           "chunks\n",
           stats_check_count, stats.n_active, stats.bytes_allocated, stats.stack_bytes_naive, stats.stack_chunk_count);
  }

  /* Basic count invariants */
  assert(stats.n_active >= 1); /* at least main */
  assert(stats.n_total >= stats.n_active);

  /* Memory allocation invariants */
  assert(stats.blocks_allocated_total >= stats.blocks_allocated);
  assert(stats.bytes_allocated_peak >= stats.bytes_allocated);
  assert(stats.blocks_allocated_peak >= stats.blocks_allocated);

  /* Stack storage invariants */
  if (stats.stack_count > 0) {
    /* Chunks must be at least as many as stacks (each stack has at least one
     * chunk) */
    assert(stats.stack_chunk_count >= stats.stack_count);

    /* Expanded size accounts for all chunks with overhead */
    /* It should match the tracked bytes exactly */
    assert(stats.stack_bytes_expanded == stats.stack_bytes);

    /* Relationship: actual <= expanded <= naive (approximately) */
    /* - actual (stack_bytes): current usage with chunk sharing enabled */
    /* - expanded (stack_bytes_expanded): usage if each tealet had its own
     * chunks (no sharing) */
    /* - naive (stack_bytes_naive): usage if we saved full stack extents */
    /* Generally: actual <= expanded <= naive */
    assert(stats.stack_bytes <= stats.stack_bytes_expanded);

    /* Expanded can be larger than naive due to chunk overhead */
    /* Each stack has one initial chunk (embedded in tealet_stack_t structure)
     */
    /* Additional chunks beyond the first add overhead for the chunk header */
    /* Chunk header contains: next pointer, stack_near pointer, size field,
     * and refcount. Header size is architecture/alignment dependent, so we
     * compute an aligned overhead estimate below. */
    /*
     * Example: Stack with 2 chunks saving 288 bytes total
     *   - naive = 64 (struct overhead) + 288 (extent) = 352 bytes
     *   - expanded = base + data + one extra chunk header + extra chunk data
     *   - difference = one extra chunk header
     */
    size_t extra_chunks = stats.stack_chunk_count - stats.stack_count;
    size_t chunk_header_raw = sizeof(void *) * 2 + sizeof(size_t) + sizeof(int);
    size_t chunk_header_align = sizeof(void *);
    size_t chunk_header_overhead =
        ((chunk_header_raw + chunk_header_align - 1) / chunk_header_align) * chunk_header_align;
    size_t max_overhead = extra_chunks * chunk_header_overhead;
    if (stats.stack_bytes_expanded > stats.stack_bytes_naive + max_overhead) {
      fprintf(stderr,
              "INVARIANT FAIL: expanded=%zu > naive=%zu + overhead=%zu "
              "(extra_chunks=%zu)\n",
              stats.stack_bytes_expanded, stats.stack_bytes_naive, max_overhead, extra_chunks);
      fflush(stderr);
    }
    assert(stats.stack_bytes_expanded <= stats.stack_bytes_naive + max_overhead);
  } else {
    /* No stacks means all counters should be zero */
    assert(stats.stack_bytes == 0);
    assert(stats.stack_bytes_expanded == 0);
    assert(stats.stack_bytes_naive == 0);
    assert(stats.stack_chunk_count == 0);
  }
}

void print_final_stats(void) {
  tealet_stats_t stats;

  if (!g_stats_enabled)
    return;

  tealet_get_stats(g_main, &stats);

  printf("  Final: %d checks, peak %zu bytes, current %zu bytes allocated\n", stats_check_count, stats_max_allocated,
         stats.bytes_allocated);
  printf("    Naive: max %zu bytes, current %zu bytes\n", stats_max_naive, stats.stack_bytes_naive);

  /* Show chunk sharing statistics */
  if (stats.stack_chunk_count > 0 && stats.stack_count > 0) {
    double sharing_ratio = (double)stats.stack_bytes_expanded / stats.stack_bytes;
    printf("  Stack sharing: %zu stacks in %zu chunks (%.1fx expansion)\n", stats.stack_count, stats.stack_chunk_count,
           sharing_ratio);
  }

  if (stats.stack_bytes > 0 && stats.stack_bytes_naive > 0) {
    double current_ratio = (double)stats.stack_bytes_naive / (double)stats.stack_bytes;
    printf("    Current ratio: %.2fx (stack-slicing using %.1f%% of naive)\n", current_ratio, 100.0 / current_ratio);
  }

  /* Reset for next test */
  stats_check_count = 0;
  stats_max_allocated = 0;
  stats_max_naive = 0;
}

tealet_alloc_t talloc = TEALET_ALLOC_INIT_MALLOC;
int talloc_fail = 0;

/* Capture cumulative callback counters so we can assert transition deltas
 * around a single API operation.
 */
void lock_snapshot_take(lock_snapshot_t *snap) {
  snap->lock_calls = g_lock_state.lock_calls;
  snap->unlock_calls = g_lock_state.unlock_calls;
}

/* Assert one lock/unlock pair occurred since the snapshot. */
void lock_snapshot_assert_delta_one(const lock_snapshot_t *before) {
  assert(g_lock_state.lock_calls - before->lock_calls == 1);
  assert(g_lock_state.unlock_calls - before->unlock_calls == 1);
}

void test_lock_assert_unheld(void) { tealet_test_lock_assert_unheld(&g_lock_state); }

void init_test_locking(void) {
  assert(g_main != NULL);
  tealet_test_lock_install(g_main, &g_lock_state);
  g_locking_enabled = 1;
}

void *failmalloc(size_t size, void *context) {
  if (talloc_fail)
    return 0;
  return malloc(size);
}

void init_test_extra(tealet_alloc_t *alloc, size_t extrasize) {
  tealet_stats_t stats;
  int result;
  assert(g_main == NULL);
  talloc.malloc_p = failmalloc;
  if (alloc == NULL)
    alloc = &talloc;
  g_main = tealet_initialize(alloc, extrasize);
  result = tealet_configure_check_stack(g_main, 0);
  assert(result == 0);
  assert(tealet_current(g_main) == g_main);
  if (extrasize)
    assert(g_main->extra != NULL);
  else
    assert(g_main->extra == NULL);

  /* Detect if stats are enabled at runtime */
  tealet_get_stats(g_main, &stats);
  g_stats_enabled = (stats.blocks_allocated > 0);

  status = 0;
}

void init_test() { init_test_extra(NULL, 0); }

void fini_test() {
  tealet_stats_t stats;
  assert(g_main != NULL);
  assert(tealet_current(g_main) == g_main);
  if (the_stub)
    tealet_delete(the_stub);
  the_stub = NULL;
  tealet_get_stats(g_main, &stats);
  if (g_stats_enabled) {
    assert(stats.n_active == 1); /* main tealet  only */
  }
  if (g_locking_enabled)
    tealet_test_lock_assert_balanced(&g_lock_state);
  tealet_finalize(g_main);
  g_main = NULL;
  g_locking_enabled = 0;
}

/**************************************/

// create a tealet, either deferred or immediate via tealet_spawn flags
static int tealet_new_x(tealet_t *m, tealet_t **out, tealet_run_t run, void **parg, void *stack_far) {
  static int counter = 0;
  int i;
  int rc;
  tealet_t *r;

  counter += 1;
  if (counter % 2) {
    r = NULL;
    rc = tealet_spawn(m, &r, run, parg, stack_far, TEALET_START_SWITCH);
    if (rc != 0)
      return rc;
    *out = r;
    return 0;
  }

  r = NULL;
  rc = tealet_spawn(m, &r, run, NULL, stack_far, TEALET_START_DEFAULT);
  if (rc != 0)
    return rc;
  i = tealet_switch(r, parg, TEALET_XFER_DEFAULT);
  if (i != 0) {
    tealet_delete(r);
    return i;
  }
  *out = r;
  return 0;
}

tealet_t *tealet_new_native_call(tealet_t *m, tealet_run_t run, void **parg, void *stack_far) {
  tealet_t *r = NULL;
  int rc = tealet_spawn(m, &r, run, parg, stack_far, TEALET_START_SWITCH);
  if (rc != 0)
    return NULL;
  return r;
}

/* create a tealet or stub low on the stack */
int tealet_new_descend(tealet_t *t, tealet_t **out, int level, tealet_run_t run, void **parg, void *stack_far) {
  int boo[10];
  boo[9] = 0;
  (void)boo;
  if (level > 0)
    return tealet_new_descend(t, out, level - 1, run, parg, stack_far);
  if (run)
    return tealet_new_x(t, out, run, parg, stack_far);
  return tealet_stub_new(t, out, stack_far);
}

/***************************************
 * methods for creating tealets in different ways
 */

static int tealet_new_rnd(tealet_t *t, tealet_t **out, tealet_run_t run, void **parg, void *stack_far) {
  return tealet_new_descend(t, out, rand() % 20, run, parg, stack_far);
}

static int stub_new(tealet_t *t, tealet_t **out, tealet_run_t run, void **parg, void *stack_far) {
  tealet_t *stub = NULL;
  int rc;
  int res;
  rc = tealet_new_descend(t, &stub, rand() % 20, NULL, NULL, stack_far);
  if (rc != 0)
    return rc;
  if (run)
    res = tealet_stub_run(stub, run, parg);
  else
    res = 0;
  if (res) {
    tealet_delete(stub);
    assert(res == TEALET_ERR_MEM);
    return res;
  }
  *out = stub;
  return 0;
}

static int stub_new2(tealet_t *t, tealet_t **out, tealet_run_t run, void **parg, void *stack_far) {
  tealet_t *dup, *stub;
  int rc;
  int res;
  stub = NULL;
  rc = tealet_new_descend(t, &stub, rand() % 20, NULL, NULL, stack_far);
  if (rc != 0)
    return rc;
  dup = tealet_duplicate(stub);
  if (dup == NULL) {
    tealet_delete(stub);
    return TEALET_ERR_MEM;
  }
  if (run)
    res = tealet_stub_run(dup, run, parg);
  else
    res = 0;
  tealet_delete(stub);
  if (res) {
    tealet_delete(dup);
    assert(res == TEALET_ERR_MEM);
    return res;
  }
  *out = dup;
  return 0;
}

static int stub_new3(tealet_t *t, tealet_t **out, tealet_run_t run, void **parg, void *stack_far) {
  tealet_t *dup;
  int rc;
  int res;
  if ((rand() % 10) == 0)
    if (the_stub != NULL) {
      tealet_delete(the_stub);
      the_stub = NULL;
    }
  if (the_stub == NULL) {
    rc = tealet_new_descend(t, &the_stub, rand() % 20, NULL, NULL, stack_far);
    if (rc != 0)
      return rc;
  }
  if (the_stub == NULL)
    return TEALET_ERR_MEM;
  dup = tealet_duplicate(the_stub);
  if (dup == NULL)
    return TEALET_ERR_MEM;
  if (run) {
    res = tealet_stub_run(dup, run, parg);
    if (res) {
      tealet_delete(dup);
      assert(res == TEALET_ERR_MEM);
      return res;
    }
  }
  *out = dup;
  return 0;
}

typedef int (*t_new)(tealet_t *, tealet_t **, tealet_run_t, void **, void *);
static t_new newarray[] = {tealet_new_rnd, stub_new, stub_new2, stub_new3};

static t_new get_new() {
  if (newmode >= 0)
    return newarray[newmode];
  return newarray[rand() % (sizeof(newarray) / sizeof(*newarray))];
}
/* Explicitly named test-only creator dispatch to avoid shadowing public APIs. */
#define TEALET_TEST_NEW(T, O, R, A, S) (get_new()((T), (O), (R), (A), (S)))

int tealet_test_new_dispatch(tealet_t *t, tealet_t **out, tealet_run_t run, void **parg, void *stack_far) {
  return TEALET_TEST_NEW(t, out, run, parg, stack_far);
}

typedef struct test_entry_t {
  const char *name;
  void (*fn)(void);
} test_entry_t;

static test_entry_t test_list[] = {
    {"test_main_current", test_main_current},
    {"test_set_far_non_main_invalid", test_set_far_non_main_invalid},
    {"test_stack_further", test_stack_further},
    {"test_stack_far_isolation", test_stack_far_isolation},
    {"test_add_unbound_phase1", test_add_unbound_phase1},
    {"test_simple", test_simple},
    {"test_lock_transitions", test_lock_transitions},
    {"test_lock_transitions_stub", test_lock_transitions_stub},
    {"test_lock_transitions_fork", test_lock_transitions_fork},
    {"test_simple_create", test_simple_create},
    {"test_simple_create_and_run", test_simple_create_and_run},
    {"test_create_previous", test_create_previous},
    {"test_previous_cleared_on_manual_delete", test_previous_cleared_on_manual_delete},
    {"test_status", test_status},
    {"test_exit", test_exit},
    {"test_switch", test_switch},
    {"test_switch_self_panic", test_switch_self_panic},
    {"test_switch_new", test_switch_new},
    {"test_arg", test_arg},
    {"test_random", test_random},
    {"test_random2", test_random2},
    {"test_extra", test_extra},
    {"test_memstats", test_memstats},
    {"test_stats", test_stats},
    {"test_mem_error", test_mem_error},
    {"test_oom_force_marks_source_defunct", test_oom_force_marks_source_defunct},
    {"test_oom_force_main_not_defunct", test_oom_force_main_not_defunct},
    {"test_oom_force_peer_then_panic_main", test_oom_force_peer_then_panic_main},
    {"test_switch_nofail_retries_force", test_switch_nofail_retries_force},
    {"test_switch_nofail_defunct_target_panics_main", test_switch_nofail_defunct_target_panics_main},
    {"test_exit_nofail_retries_force", test_exit_nofail_retries_force},
    {"test_exit_nofail_defunct_target_panics_main", test_exit_nofail_defunct_target_panics_main},
    {"test_exit_self_invalid", test_exit_self_invalid},
#if TEALET_WITH_TESTING
    {"test_exit_defunct_target_returns_error", test_exit_defunct_target_returns_error},
    {"test_exit_explicit_panic", test_exit_explicit_panic},
    {"test_debug_swap_far_invalid_caller_check_main", test_debug_swap_far_invalid_caller_check_main},
    {"test_debug_swap_far_invalid_caller_check_child", test_debug_swap_far_invalid_caller_check_child},
#endif
    {NULL, NULL}};

void runmode(int mode) {
  int i;
  newmode = mode;
  printf("+++ Running tests with newmode = %d\n", newmode);
  fflush(stdout);
  for (i = 0; test_list[i].fn != NULL; i++) {
    printf("+++ Running test %d (%s)... +++\n", i, test_list[i].name);
    fflush(stdout);
    test_list[i].fn();
    printf("+++ Completed test %d (%s). +++\n", i, test_list[i].name);
    fflush(stdout);
  }
  printf("+++ All ok. +++\n");
  fflush(stdout);
}

int main(int argc, char **argv) {
  int i;
  (void)argc;
  (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  for (i = 0; i <= 3; i++)
    runmode(i);
  runmode(-1);
  return 0;
}
