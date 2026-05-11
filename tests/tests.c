#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined _MSC_VER || _MSC_VER >= 1600 /* VS2010 and above */
#include <stdint.h>
#endif

#include "tealet.h"
#include "tealet_extras.h"
#include "test_harness.h"
#include "test_lock_helpers.h"
#include "test_locking.h"

#if TEALET_WITH_TESTING
/* Internal test hook from tealet.c (not part of public API). */
int tealet_debug_swap_far(tealet_t *tealet, void *new_far, void **old_far);

/* Internal test hook from tealet.c (not part of public API). */
int tealet_debug_force_defunct(tealet_t *tealet);
#endif

int status = 0;
tealet_t *g_main = NULL;
static tealet_t *the_stub = NULL;
static int newmode = 0;
tealet_test_lock_state_t g_lock_state;

/* Runtime detection of stats support */
static int g_stats_enabled = 0;
static int stats_check_count = 0;
static size_t stats_max_allocated = 0;
static size_t stats_max_naive = 0;

/* Check stats periodically during tests */
static void check_stats(int verbose) {
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

static void print_final_stats(void) {
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

static tealet_alloc_t talloc = TEALET_ALLOC_INIT_MALLOC;
static int talloc_fail = 0;

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
  tealet_test_lock_install(g_main, &g_lock_state);
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
  tealet_test_lock_assert_balanced(&g_lock_state);
  tealet_finalize(g_main);
  g_main = NULL;
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
    rc = tealet_spawn(m, &r, run, parg, stack_far, TEALET_RUN_SWITCH);
    if (rc != 0)
      return rc;
    *out = r;
    return 0;
  }

  r = NULL;
  rc = tealet_spawn(m, &r, run, NULL, stack_far, TEALET_RUN_DEFAULT);
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
  int rc = tealet_spawn(m, &r, run, parg, stack_far, TEALET_RUN_SWITCH);
  if (rc != 0)
    return NULL;
  return r;
}

/* create a tealet or stub low on the stack */
static int tealet_new_descend(tealet_t *t, tealet_t **out, int level, tealet_run_t run, void **parg, void *stack_far) {
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
static tealet_t *(*tealet_new_native)(tealet_t *, tealet_run_t, void **, void *) = tealet_new_native_call;

static t_new get_new() {
  if (newmode >= 0)
    return newarray[newmode];
  return newarray[rand() % (sizeof(newarray) / sizeof(*newarray))];
}
/* Explicitly named test-only creator dispatch to avoid shadowing public APIs. */
#define TEALET_TEST_NEW(T, O, R, A, S) (get_new()((T), (O), (R), (A), (S)))

/************************************************************/

void test_main_current(void) {
  init_test();
  fini_test();
}

static void test_stack_further_inner(void *outer_addr) {
  int inner_local;
  void *further = tealet_stack_further(outer_addr, &inner_local);
  assert(further == outer_addr);
}

typedef struct stack_far_case_t {
  int value;
} stack_far_case_t;

typedef struct stack_far_run_arg_t {
  tealet_t *main;
  stack_far_case_t *shared;
  int before;
  int after;
} stack_far_run_arg_t;

static tealet_t *test_stack_far_isolation_run(tealet_t *current, void *arg) {
  stack_far_run_arg_t *run_arg = (stack_far_run_arg_t *)arg;
  assert(current != run_arg->main);
  assert(run_arg->shared->value == run_arg->before);
  run_arg->shared->value = run_arg->after;
  tealet_switch(run_arg->main, NULL, TEALET_XFER_DEFAULT);
  assert(run_arg->shared->value == run_arg->after);
  return run_arg->main;
}

static tealet_t *test_stack_far_isolation_parent(tealet_t *current, void *arg) {
  stack_far_case_t shared;
  stack_far_run_arg_t *run_arg;
  tealet_t *child;
  void *child_arg;
  void *stack_far;
  (void)arg;

  shared.value = 11;
  run_arg = (stack_far_run_arg_t *)tealet_malloc(current, sizeof(*run_arg));
  assert(run_arg != NULL);
  run_arg->main = current;
  run_arg->shared = &shared;
  run_arg->before = 11;
  run_arg->after = 99;
  child_arg = run_arg;

  stack_far = tealet_stack_further(&shared, (void *)(&shared + 1));
  child = tealet_new_native(current, test_stack_far_isolation_run, &child_arg, stack_far);
  assert(child != NULL);

  /* Child already ran once during creation (RUN_SWITCH): it wrote its private
   * copy and switched back to parent.
   */
  assert(shared.value == 11);

  /* Resume child: it confirms its private value, then returns/exits to parent. */
  tealet_switch(child, NULL, TEALET_XFER_DEFAULT);

  /* Child has exited; explicit delete is still required. */
  tealet_delete(child);

  /* Parent value remains unchanged throughout. */
  assert(shared.value == 11);
  tealet_free(current, run_arg);
  return g_main;
}

void test_stack_further(void) {
  int a_local;
  int b_local;
  int outer_local;
  void *a;
  void *b;
  void *further_ab;
  void *further_ba;
  init_test();
  a = &a_local;
  b = &b_local;
  further_ab = tealet_stack_further(a, b);
  further_ba = tealet_stack_further(b, a);
  assert(further_ab == a || further_ab == b);
  assert(further_ba == a || further_ba == b);
  assert(further_ab == further_ba);
  assert(tealet_stack_further(a, a) == a);
  assert(tealet_stack_further(b, b) == b);
  assert(tealet_stack_further(further_ab, a) == further_ab);
  assert(tealet_stack_further(further_ab, b) == further_ab);

  /* Cross-frame check: caller frame local should be farther than callee local
   */
  test_stack_further_inner(&outer_local);
  fini_test();
}

/* test that stack isolation works if tealets are created with an extended
 * stack. */
void test_stack_far_isolation(void) {
  tealet_t *parent;

  init_test();
  parent = tealet_new_native(g_main, test_stack_far_isolation_parent, NULL, NULL);
  assert(parent != NULL);
  tealet_delete(parent);
  fini_test();
}

void test_add_unbound_phase1(void) {
  tealet_t *unbound;
  tealet_t *copy;
  int rc;

  init_test();
  unbound = tealet_new(g_main);
  assert(unbound != NULL);
  assert(tealet_status(unbound) == TEALET_STATUS_NEW);
  assert(tealet_get_far(unbound) == NULL);

  copy = tealet_duplicate(unbound);
  assert(copy != NULL);
  assert(tealet_status(copy) == TEALET_STATUS_NEW);
  assert(tealet_get_far(copy) == NULL);

  rc = tealet_switch(unbound, NULL, TEALET_XFER_DEFAULT);
  assert(rc == TEALET_ERR_INVAL);

  tealet_delete(copy);
  tealet_delete(unbound);
  fini_test();
}

/************************************************************/

tealet_t *test_simple_run(tealet_t *t1, void *arg) {
  tealet_t *prev_current;
  unsigned int origin_current;
  unsigned int origin_main;
  (void)arg;
  assert(t1 != g_main);
  origin_current = tealet_get_origin(t1);
  origin_main = tealet_get_origin(g_main);
  assert((origin_current & TEALET_ORIGIN_MAIN_LINEAGE) == 0);
  assert((origin_current & TEALET_ORIGIN_FORK) == 0);
  assert((origin_main & TEALET_ORIGIN_MAIN_LINEAGE) != 0);
  assert((origin_main & TEALET_ORIGIN_FORK) == 0);
  prev_current = tealet_previous(t1);
  assert(prev_current == t1->main);
  status = 1;
  return g_main;
}

void test_simple(void) {
  tealet_t *t;
  init_test();
  t = tealet_new_native_call(g_main, test_simple_run, NULL, NULL);
  assert(t != NULL);
  assert(status == 1);
  tealet_delete(t);
  fini_test();
}

void test_simple_create(void) {
  tealet_t *t;
  init_test();
  t = NULL;
  assert(tealet_spawn(g_main, &t, test_simple_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(status == 0);
  tealet_delete(t);
  fini_test();
}

void test_simple_create_and_run(void) {
  tealet_t *t;
  init_test();
  t = NULL;
  assert(tealet_spawn(g_main, &t, test_simple_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  tealet_switch(t, NULL, TEALET_XFER_DEFAULT);
  assert(status == 1);
  assert(tealet_previous(g_main) == t);
  tealet_delete(t);
  assert(tealet_previous(g_main) == NULL);
  fini_test();
}

/* Test that tealet_previous() is correct inside run function for tealet_create
 */
static tealet_t *test_create_previous_run(tealet_t *t1, void *arg) {
  /* When first switched to via tealet_switch(), previous should be main */
  assert(tealet_previous(t1) == t1->main);
  status = 42;
  return g_main;
}

void test_create_previous(void) {
  tealet_t *t;
  init_test();
  /* Create tealet without running it */
  t = NULL;
  assert(tealet_spawn(g_main, &t, test_create_previous_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(status == 0);
  /* Now switch to it - it should see main as previous */
  tealet_switch(t, NULL, TEALET_XFER_DEFAULT);
  assert(status == 42); /* Verify it ran */
  assert(tealet_previous(g_main) == t);
  tealet_delete(t);
  assert(tealet_previous(g_main) == NULL);
  fini_test();
}

static tealet_t *test_previous_manual_delete_run(tealet_t *t1, void *arg) {
  (void)arg;
  tealet_switch(t1->main, NULL, TEALET_XFER_DEFAULT);
  return t1->main;
}

void test_previous_cleared_on_manual_delete(void) {
  tealet_t *t;

  init_test();
  t = NULL;
  assert(tealet_spawn(g_main, &t, test_previous_manual_delete_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(t != NULL);

  tealet_switch(t, NULL, TEALET_XFER_DEFAULT);
  assert(tealet_previous(g_main) == t);

  tealet_delete(t);
  assert(tealet_previous(g_main) == NULL);
  fini_test();
}

/*************************************************************/

tealet_t *test_status_run(tealet_t *t1, void *arg) {
  assert(t1 == tealet_current(t1));
  assert(!TEALET_IS_MAIN(t1));
  assert(tealet_status(t1) == TEALET_STATUS_ACTIVE);
  return g_main;
}

void test_status(void) {
  tealet_t *stub1;
  int result;
  init_test();

  assert(tealet_status(g_main) == TEALET_STATUS_ACTIVE);
  assert(TEALET_IS_MAIN(g_main));

  stub1 = NULL;
  result = TEALET_TEST_NEW(g_main, &stub1, NULL, NULL, NULL);
  assert(result == 0);
  assert(stub1 != NULL);
  assert(tealet_status(stub1) == TEALET_STATUS_ACTIVE);
  assert(!TEALET_IS_MAIN(stub1));
  tealet_stub_run(stub1, test_status_run, NULL);
  tealet_delete(stub1);

  fini_test();
}

/*************************************************************/
tealet_t *test_exit_run(tealet_t *t1, void *arg) {
  int result;
  assert(t1 != g_main);
  status += 1;
  result = tealet_exit(g_main, NULL, (int)(intptr_t)arg);
  abort();
  assert(result == 0);
  return (tealet_t *)-1;
}

void test_exit(void) {
  tealet_t *stub1, *stub2;
  int result;
  void *arg;
  init_test();
  stub1 = NULL;
  result = TEALET_TEST_NEW(g_main, &stub1, NULL, NULL, NULL);
  assert(result == 0);
  assert(stub1 != NULL);
  stub2 = tealet_duplicate(stub1);
  arg = (void *)TEALET_XFER_DEFAULT;
  result = tealet_stub_run(stub1, test_exit_run, &arg);
  assert(result == 0);
  assert(status == 1);
  assert(tealet_status(stub1) == TEALET_STATUS_EXITED);
  tealet_delete(stub1);
  arg = (void *)TEALET_EXIT_DELETE;
  result = tealet_stub_run(stub2, test_exit_run, &arg);
  assert(status == 2);
  fini_test();
}

/************************************************************/

static tealet_t *glob_t1;
static tealet_t *glob_t2;

tealet_t *test_switch_2(tealet_t *t2, void *arg) {
  assert(t2 != g_main);
  assert(t2 != glob_t1);
  glob_t2 = t2;
  assert(status == 1);
  status = 2;
  assert(tealet_current(g_main) == t2);
  tealet_switch(glob_t1, NULL, TEALET_XFER_DEFAULT);
  assert(status == 3);
  status = 4;
  assert(tealet_current(g_main) == t2);
  tealet_switch(glob_t1, NULL, TEALET_XFER_DEFAULT);
  assert(status == 5);
  status = 6;
  assert(t2 == glob_t2);
  assert(tealet_current(g_main) == t2);
  tealet_switch(t2, NULL, TEALET_XFER_DEFAULT);
  assert(status == 6);
  status = 7;
  assert(tealet_current(g_main) == t2);
  return g_main;
}

tealet_t *test_switch_1(tealet_t *t1, void *arg) {
  assert(t1 != g_main);
  glob_t1 = t1;
  assert(status == 0);
  status = 1;
  assert(tealet_current(g_main) == t1);
  assert(tealet_new_native_call(g_main, test_switch_2, NULL, NULL) != NULL);
  assert(status == 2);
  status = 3;
  assert(tealet_current(g_main) == t1);
  tealet_switch(glob_t2, NULL, TEALET_XFER_DEFAULT);
  assert(status == 4);
  status = 5;
  assert(tealet_current(g_main) == t1);
  return glob_t2;
}

void test_switch(void) {
  init_test();
  assert(tealet_new_native_call(g_main, test_switch_1, NULL, NULL) != NULL);
  assert(status == 7);
  tealet_delete(glob_t1);
  tealet_delete(glob_t2);
  fini_test();
}

void test_switch_self_panic(void) {
  tealet_t *runner;
  int result;

  /* Purpose: self-switch with PANIC should return TEALET_ERR_PANIC
   * immediately and must not leak panic state into later switches.
   */
  init_test();

  /* Self-switch with PANIC should consume panic immediately. */
  result = tealet_switch(g_main, NULL, TEALET_XFER_PANIC);
  assert(result == TEALET_ERR_PANIC);

  /* Ensure panic flag is not left armed for a later unrelated switch. */
  runner = tealet_new_native_call(g_main, test_simple_run, NULL, NULL);
  assert(runner != NULL);
  assert(status == 1);
  tealet_delete(runner);

  fini_test();
}

/************************************************************/

/* 1 is high on the stack.  We then create 2 lower on the stack */
/* the execution is : m 1 m 2 1 m 2 m */
tealet_t *test_switch_new_1(tealet_t *t1, void *arg) {
  tealet_t *caller = (tealet_t *)arg;
  tealet_t *stub;
  int result;
  /* switch back to the creator */
  tealet_switch(caller, NULL, TEALET_XFER_DEFAULT);
  /* now we want to trample the stack */
  stub = NULL;
  result = tealet_new_descend(t1, &stub, 50, NULL, NULL, NULL);
  assert(result == 0);
  assert(stub != NULL);
  tealet_delete(stub);
  /* and back to main */
  return g_main;
}

tealet_t *test_switch_new_2(tealet_t *t2, void *arg) {
  tealet_t *target = (tealet_t *)arg;
  /* switch to tealet 1 to trample the stack*/
  target->extra = (void *)t2;
  tealet_switch(target, NULL, TEALET_XFER_DEFAULT);

  /* and then return to main */
  return g_main;
}

void test_switch_new(void) {
  tealet_t *tealet1, *tealet2;
  void *arg;
  int result;
  init_test();
  arg = (void *)tealet_current(g_main);
  tealet1 = tealet_new_native_call(g_main, test_switch_new_1, &arg, NULL);
  /* the tealet is now running */
  arg = (void *)tealet1;
  tealet2 = NULL;
  result = tealet_new_descend(g_main, &tealet2, 4, test_switch_new_2, &arg, NULL);
  assert(result == 0);
  assert(tealet2 != NULL);
  assert(tealet_status(tealet2) == TEALET_STATUS_ACTIVE);
  tealet_switch(tealet2, NULL, TEALET_XFER_DEFAULT);
  tealet_delete(tealet1);
  tealet_delete(tealet2);
  fini_test();
}

/************************************************************/

/* test argument passing with switch and exit */
tealet_t *test_arg_1(tealet_t *t1, void *arg) {
  void *myarg;
  tealet_t *peer = (tealet_t *)arg;
  myarg = (void *)1;
  tealet_switch(peer, &myarg, TEALET_XFER_DEFAULT);
  assert(myarg == (void *)2);
  myarg = (void *)3;
  tealet_exit(peer, myarg, TEALET_EXIT_DELETE);
  return NULL;
}

void test_arg(void) {
  void *myarg;
  tealet_t *t1;
  init_test();
  myarg = (void *)g_main;
  t1 = tealet_new_native_call(g_main, test_arg_1, &myarg, NULL);
  assert(myarg == (void *)1);
  myarg = (void *)2;
  tealet_switch(t1, &myarg, TEALET_XFER_DEFAULT);
  assert(myarg == (void *)3);
  fini_test();
}

/************************************************************/

#define ARRAYSIZE 127
#define MAX_STATUS 50000

static tealet_t *tealetarray[ARRAYSIZE] = {NULL};
static int got_index;

tealet_t *random_new_tealet(tealet_t *, void *arg);

static void random_run(int index) {
  int i, prevstatus;
  void *arg;
  tealet_t *cur = tealet_current(g_main);
  assert(tealetarray[index] == cur);
  do {
    i = rand() % (ARRAYSIZE + 1);
    status += 1;

    /* Check stats periodically */
    if (status % 100 == 0)
      check_stats(0);

    if (i == ARRAYSIZE)
      break;
    prevstatus = status;
    got_index = i;
    if (tealetarray[i] == NULL) {
      if (status >= MAX_STATUS)
        break;
      arg = (void *)(intptr_t)i;
      assert(tealet_new_native_call(g_main, random_new_tealet, &arg, NULL) != NULL);
    } else {
      tealet_switch(tealetarray[i], NULL, TEALET_XFER_DEFAULT);
    }
    assert(status >= prevstatus);
    assert(tealet_current(g_main) == cur);
    assert(tealetarray[index] == cur);
    assert(got_index == index);
  } while (status < MAX_STATUS);
}

tealet_t *random_new_tealet(tealet_t *cur, void *arg) {
  int i = got_index;
  assert(tealet_current(g_main) == cur);
  assert(i == (intptr_t)(arg));
  assert(i > 0 && i < ARRAYSIZE);
  assert(tealetarray[i] == NULL);
  tealetarray[i] = cur;
  random_run(i);
  tealetarray[i] = NULL;

  i = rand() % ARRAYSIZE;
  if (tealetarray[i] == NULL) {
    assert(tealetarray[0] != NULL);
    i = 0;
  }
  got_index = i;
  tealet_exit(tealetarray[i], NULL, TEALET_EXIT_DELETE);
  abort();
  return NULL;
}

void test_random(void) {
  int i;
  init_test();
  for (i = 0; i < ARRAYSIZE; i++)
    tealetarray[i] = NULL;
  tealetarray[0] = g_main;
  status = 0;
  while (status < MAX_STATUS)
    random_run(0);

  assert(g_main == tealetarray[0]);
  for (i = 1; i < ARRAYSIZE; i++)
    while (tealetarray[i] != NULL)
      random_run(0);

  print_final_stats();
  fini_test();
}

/************************************************************/

/* Another random switching test.  Tealets find a random target
 * tealet to switch to and do that.  While the test is running, they
 * generate new tealets to fill the array.  Each tealet runs x times
 * and then exits.  The switching happens at a random depth.
 */
#define N_RUNS 10
#define MAX_DESCEND 20

void random2_run(int index);
tealet_t *random2_tealet(tealet_t *cur, void *arg) {
  int index = (int)(intptr_t)arg;
  assert(tealet_current(g_main) == cur);
  assert(index > 0 && index < ARRAYSIZE);
  assert(tealetarray[index] == NULL);
  tealetarray[index] = cur;
  random2_run(index);
  tealetarray[index] = NULL;
  tealet_exit(tealetarray[0], NULL, TEALET_EXIT_DELETE); /* switch to main */
  abort();
  return NULL;
}
void random2_new(int index) {
  void *arg = (void *)(intptr_t)index;
  assert(tealet_new_native_call(g_main, random2_tealet, &arg, NULL) != NULL);
}

int random2_descend(int index, int level) {
  int target;
  if (level > 0)
    return random2_descend(index, level - 1);

  /* find target */
  target = rand() % ARRAYSIZE;
  if (status < MAX_STATUS) {
    status += 1;
    while (target == index)
      target = rand() % ARRAYSIZE;
    if (tealetarray[target] == NULL)
      random2_new(target);
    else
      tealet_switch(tealetarray[target], NULL, TEALET_XFER_DEFAULT);
    return 1;
  } else {
    /* find a telet */
    int j;
    for (j = 0; j < ARRAYSIZE; j++) {
      int k = (j + target) % ARRAYSIZE;
      if (k != index && tealetarray[k]) {
        status += 1;
        tealet_switch(tealetarray[k], NULL, TEALET_XFER_DEFAULT);
        return 1;
      }
    }
    return 0;
  }
}

void random2_run(int index) {
  int i;
  assert(tealetarray[index] == NULL || tealetarray[index] == tealet_current(g_main));
  tealetarray[index] = tealet_current(g_main);
  for (i = 0; i < N_RUNS; i++) {
    /* Check stats periodically */
    if (status % 100 == 0)
      check_stats(0);

    if (random2_descend(index, rand() % (MAX_DESCEND + 1)) == 0)
      break;
  }
  tealetarray[index] = NULL;
}

void test_random2(void) {
  int i;
  init_test();
  for (i = 0; i < ARRAYSIZE; i++)
    tealetarray[i] = NULL;
  tealetarray[0] = g_main;

  while (status < MAX_STATUS)
    random2_run(0);

  /* drain the system */
  tealetarray[0] = tealet_current(g_main);
  for (;;) {
    for (i = 1; i < ARRAYSIZE; i++)
      if (tealetarray[i]) {
        status++;
        tealet_switch(tealetarray[i], NULL, TEALET_XFER_DEFAULT);
        break;
      }
    if (i == ARRAYSIZE)
      break;
  }
  tealetarray[0] = NULL;
  print_final_stats();
  fini_test();
}

typedef struct extradata {
  int foo;
  char bar[5];
  int gaz;
} extradata;

tealet_t *extra_tealet(tealet_t *cur, void *arg) {
  extradata ed2 = {1, "abcd", 2};
  extradata *ed1 = TEALET_EXTRA(cur, extradata);
  assert(ed1->foo == ed2.foo);
  assert(strcmp(ed1->bar, ed2.bar) == 0);
  assert(ed1->gaz == ed2.gaz);
  return g_main;
}

void test_extra(void) {
  tealet_t *t1, *t2;
  extradata ed = {1, "abcd", 2};
  int result;
  init_test_extra(NULL, sizeof(extradata));
  *TEALET_EXTRA(g_main, extradata) = ed;

  t1 = NULL;
  result = TEALET_TEST_NEW(g_main, &t1, NULL, NULL, NULL);
  assert(result == 0);
  assert(t1 != NULL);
  *TEALET_EXTRA(t1, extradata) = ed;
  t2 = tealet_duplicate(t1);
  tealet_stub_run(t1, extra_tealet, NULL);
  tealet_stub_run(t2, extra_tealet, NULL);
  tealet_delete(t2);
  tealet_delete(t1);
  fini_test();
}

void test_memstats(void) {
  tealet_statsalloc_t salloc;
  tealet_statsalloc_init(&salloc, &talloc);
  assert(salloc.n_allocs == 0);
  assert(salloc.s_allocs == 0);
  init_test_extra(&salloc.alloc, 0);
  assert(salloc.n_allocs > 0);
  assert(salloc.s_allocs > 0);
  fini_test();
}

void test_stats(void) {
  tealet_t *t1;
  tealet_stats_t stats;
  int a, b;
  int result;
  init_test_extra(NULL, 0);

  /* Skip this test if stats are not enabled */
  if (!g_stats_enabled) {
    fini_test();
    return;
  }

  tealet_get_stats(g_main, &stats);
  assert(stats.n_active == 1);
  assert(stats.n_total == 1);
  t1 = NULL;
  result = TEALET_TEST_NEW(g_main, &t1, NULL, NULL, NULL);
  assert(result == 0);
  assert(t1 != NULL);
  tealet_get_stats(g_main, &stats);
  /* can be more than 2 because of stub tealet */
  a = stats.n_active;
  b = stats.n_total;
  assert(a >= 2);
  assert(b >= a); /* can be bigger if tmp stub was created */
  tealet_delete(t1);
  tealet_get_stats(g_main, &stats);
  assert(stats.n_active == a - 1);
  assert(stats.n_total == b);
  fini_test();
}

tealet_t *mem_error_tealet(tealet_t *t1, void *arg) {
  void *myarg;
  int res;
  tealet_t *peer = (tealet_t *)arg;
  talloc_fail = 1;
  res = tealet_switch(peer, &myarg, TEALET_XFER_DEFAULT);
  assert(res == TEALET_ERR_MEM);
  tealet_exit(peer, myarg, TEALET_EXIT_DELETE);
  abort(); // never runs
  return NULL;
}

void test_mem_error(void) {
  void *myarg;
  tealet_t *t1;
  init_test_extra(NULL, 0);
  myarg = (void *)g_main;
  t1 = tealet_new_native_call(g_main, mem_error_tealet, &myarg, NULL);
  assert(t1);
  talloc_fail = 0;
  fini_test();
}

static tealet_t *oom_force_to_main_run(tealet_t *current, void *arg) {
  int result;
  (void)arg;

  /* First attempt without FORCE fails in-place with MEM. */
  talloc_fail = 1;
  result = tealet_switch(current->main, NULL, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_MEM);

  /* FORCE should continue by defuncting current and transferring out. */
  result = tealet_switch(current->main, NULL, TEALET_XFER_FORCE);
  abort();
  assert(result == 0);
  return NULL;
}

void test_oom_force_marks_source_defunct(void) {
  tealet_t *worker;
  int result;

  /* Purpose: under OOM, FORCE switch-to-main should succeed by marking the
   * source worker defunct. The defunct worker must reject future switches.
   */
  init_test_extra(NULL, 0);

  worker = NULL;
  assert(tealet_spawn(g_main, &worker, oom_force_to_main_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(worker != NULL);

  result = tealet_switch(worker, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);

  talloc_fail = 0;
  assert(tealet_status(worker) == TEALET_STATUS_DEFUNCT);
  result = tealet_switch(worker, NULL, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_DEFUNCT);

  tealet_delete(worker);
  fini_test();
}

static tealet_t *oom_main_probe_run(tealet_t *current, void *arg) {
  (void)arg;
  return current->main;
}

void test_oom_force_main_not_defunct(void) {
  tealet_t *worker;
  int result;

  /* Purpose: when FORCE is requested from main and save fails under OOM,
   * main must not be marked defunct; the operation returns TEALET_ERR_MEM.
   */
  init_test_extra(NULL, 0);

  worker = NULL;
  assert(tealet_spawn(g_main, &worker, oom_main_probe_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(worker != NULL);

  talloc_fail = 1;
  result = tealet_switch(worker, NULL, TEALET_XFER_FORCE);
  assert(result == TEALET_ERR_MEM);

  talloc_fail = 0;
  assert(tealet_status(g_main) == TEALET_STATUS_ACTIVE);
  assert(tealet_status(worker) != TEALET_STATUS_DEFUNCT);

  tealet_delete(worker);
  fini_test();
}

static tealet_t *oom_w2 = NULL;

static tealet_t *oom_force_peer_to_main_panic_run(tealet_t *current, void *arg) {
  int result;
  (void)arg;

  /* Clear allocator fault so this switch-out can complete. */
  talloc_fail = 0;
  result = tealet_switch(current->main, NULL, TEALET_XFER_PANIC);
  abort();
  assert(result == TEALET_ERR_PANIC);
  return NULL;
}

static tealet_t *oom_force_to_peer_run(tealet_t *current, void *arg) {
  int result;
  (void)arg;

  assert(oom_w2 != NULL);

  talloc_fail = 1;
  result = tealet_switch(oom_w2, NULL, TEALET_XFER_FORCE);
  abort();
  assert(result == 0);
  (void)current;
  return NULL;
}

void test_oom_force_peer_then_panic_main(void) {
  tealet_t *w1;
  int result;

  /* Purpose: with two workers, OOM during w1->w2 with FORCE should defunct w1,
   * continue to w2, and then panic-tagged switch to main should surface as
   * TEALET_ERR_PANIC on main.
   */
  init_test_extra(NULL, 0);

  oom_w2 = NULL;
  assert(tealet_spawn(g_main, &oom_w2, oom_force_peer_to_main_panic_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(oom_w2 != NULL);

  w1 = NULL;
  assert(tealet_spawn(g_main, &w1, oom_force_to_peer_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(w1 != NULL);

  result = tealet_switch(w1, NULL, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_PANIC);

  assert(tealet_status(w1) == TEALET_STATUS_DEFUNCT);
  result = tealet_switch(w1, NULL, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_DEFUNCT);

  tealet_delete(w1);
  tealet_delete(oom_w2);
  oom_w2 = NULL;
  talloc_fail = 0;
  fini_test();
}

static tealet_t *switch_nofail_mem_run(tealet_t *current, void *arg) {
  (void)arg;

  /* Purpose: NOFAIL should retry with FORCE and transfer to main under OOM. */
  talloc_fail = 1;
  tealet_switch(current->main, NULL, TEALET_XFER_NOFAIL);
  abort();
  return NULL;
}

void test_switch_nofail_retries_force(void) {
  tealet_t *worker;
  int result;

  init_test_extra(NULL, 0);

  worker = NULL;
  assert(tealet_spawn(g_main, &worker, switch_nofail_mem_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(worker != NULL);

  result = tealet_switch(worker, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);

  talloc_fail = 0;
  assert(tealet_status(worker) == TEALET_STATUS_DEFUNCT);
  tealet_delete(worker);
  fini_test();
}

static tealet_t *switch_nofail_defunct_target_run(tealet_t *current, void *arg) {
  tealet_t *target = (tealet_t *)arg;

  tealet_switch(target, NULL, TEALET_XFER_NOFAIL);
  abort();
  (void)current;
  return NULL;
}

void test_switch_nofail_defunct_target_panics_main(void) {
  tealet_t *victim;
  tealet_t *switcher;
  void *arg;
  int result;

  init_test_extra(NULL, 0);

  victim = NULL;
  assert(tealet_spawn(g_main, &victim, oom_force_to_main_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(victim != NULL);

  result = tealet_switch(victim, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);
  talloc_fail = 0;
  assert(tealet_status(victim) == TEALET_STATUS_DEFUNCT);

  switcher = NULL;
  assert(tealet_spawn(g_main, &switcher, switch_nofail_defunct_target_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(switcher != NULL);
  arg = (void *)victim;
  result = tealet_switch(switcher, &arg, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_PANIC);

  tealet_delete(switcher);
  tealet_delete(victim);
  fini_test();
}

static tealet_t *exit_nofail_mem_run(tealet_t *current, void *arg) {
  (void)arg;

  /* Purpose: NOFAIL should retry with FORCE and transfer to main under OOM. */
  talloc_fail = 1;
  tealet_exit(current->main, NULL, TEALET_XFER_NOFAIL);
  abort();
  return NULL;
}

void test_exit_nofail_retries_force(void) {
  tealet_t *worker;
  int result;

  init_test_extra(NULL, 0);

  worker = NULL;
  assert(tealet_spawn(g_main, &worker, exit_nofail_mem_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(worker != NULL);

  result = tealet_switch(worker, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);

  talloc_fail = 0;
  assert(tealet_status(worker) != TEALET_STATUS_ACTIVE);
  tealet_delete(worker);
  fini_test();
}

static tealet_t *exit_nofail_defunct_target_run(tealet_t *current, void *arg) {
  tealet_t *target = (tealet_t *)arg;

  tealet_exit(target, NULL, TEALET_XFER_NOFAIL);
  abort();
  (void)current;
  return NULL;
}

void test_exit_nofail_defunct_target_panics_main(void) {
  tealet_t *victim;
  tealet_t *exiter;
  void *arg;
  int result;

  init_test_extra(NULL, 0);

  victim = NULL;
  assert(tealet_spawn(g_main, &victim, oom_force_to_main_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(victim != NULL);

  result = tealet_switch(victim, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);
  talloc_fail = 0;
  assert(tealet_status(victim) == TEALET_STATUS_DEFUNCT);

  exiter = NULL;
  assert(tealet_spawn(g_main, &exiter, exit_nofail_defunct_target_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(exiter != NULL);
  arg = (void *)victim;
  result = tealet_switch(exiter, &arg, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_PANIC);

  tealet_delete(exiter);
  tealet_delete(victim);
  fini_test();
}

static tealet_t *test_exit_self_invalid_run(tealet_t *current, void *arg) {
  int result;
  (void)arg;

  result = tealet_exit((tealet_t *)current, NULL, TEALET_EXIT_DELETE);
  assert(result == TEALET_ERR_INVAL);

  tealet_exit(current->main, NULL, TEALET_EXIT_DELETE);
  abort();
  return NULL;
}

void test_exit_self_invalid(void) {
  tealet_t *runner;

  /* Purpose: exiting to self is invalid and must report TEALET_ERR_INVAL
   * (not TEALET_ERR_DEFUNCT).
   */
  init_test();

  runner = NULL;
  assert(tealet_spawn(g_main, &runner, test_exit_self_invalid_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(runner != NULL);
  assert(tealet_switch(runner, NULL, TEALET_XFER_DEFAULT) == 0);

  fini_test();
}

#if TEALET_WITH_TESTING
static tealet_t *test_exit_defunct_fail_run(tealet_t *current, void *arg) {
  tealet_t *target = (tealet_t *)arg;
  int result;

  assert(current != g_main);
  result = tealet_exit(target, NULL, TEALET_EXIT_DELETE);
  assert(result == TEALET_ERR_DEFUNCT);

  tealet_exit(current->main, NULL, TEALET_EXIT_DELETE);
  abort();
  return NULL;
}

void test_exit_defunct_target_returns_error(void) {
  tealet_t *victim;
  tealet_t *exiter;
  int result;
  void *arg;

  init_test();

  victim = NULL;
  result = TEALET_TEST_NEW(g_main, &victim, NULL, NULL, NULL);
  assert(result == 0);
  assert(victim != NULL);
  result = tealet_debug_force_defunct(victim);
  assert(result == 0);

  exiter = NULL;
  assert(tealet_spawn(g_main, &exiter, test_exit_defunct_fail_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(exiter != NULL);
  arg = (void *)victim;
  result = tealet_switch(exiter, &arg, TEALET_XFER_DEFAULT);
  assert(result == 0);

  tealet_delete(victim);
  fini_test();
}

static tealet_t *test_explicit_panic_exit_run(tealet_t *current, void *arg) {
  tealet_t *target = (tealet_t *)arg;

  assert(current != g_main);
  tealet_exit(target, NULL, TEALET_EXIT_DELETE | TEALET_XFER_PANIC);
  abort();
  return NULL;
}

void test_exit_explicit_panic(void) {
  tealet_t *exiter;
  int result;
  void *arg;

  init_test();

  exiter = NULL;
  assert(tealet_spawn(g_main, &exiter, test_explicit_panic_exit_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(exiter != NULL);
  arg = (void *)g_main;
  result = tealet_switch(exiter, &arg, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_PANIC);

  fini_test();
}

void test_debug_swap_far_invalid_caller_check_main(void) {
  tealet_t *child;
  void *old_far;
  void *new_far;
  char stack_probe;
  int result;

  /* Test purpose: validate caller-check failure on the main tealet.
   * We move main->stack_far to current stack probe + 64 MiB so the
   * max-stack-distance check fails deterministically, then restore it.
   */
  init_test();

  child = NULL;
  result = TEALET_TEST_NEW(g_main, &child, NULL, NULL, NULL);
  assert(result == 0);
  assert(child != NULL);

  new_far = (void *)((uintptr_t)&stack_probe + ((uintptr_t)64 << 20));
  result = tealet_debug_swap_far(g_main, new_far, &old_far);
  assert(result == 0);

  result = tealet_switch(child, NULL, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_INVAL);

  result = tealet_debug_swap_far(g_main, old_far, &old_far);
  assert(result == 0);

  tealet_delete(child);
  fini_test();
}

static tealet_t *test_invalid_caller_check_child_run(tealet_t *current, void *arg) {
  void *old_far;
  void *new_far;
  char stack_probe;
  int result;
  (void)arg;

  /* Test purpose: validate caller-check failure on a running child tealet.
   * We move child->stack_far to current stack probe + 64 MiB so switching to
   * main fails with TEALET_ERR_INVAL, then restore and exit normally.
   */
  new_far = (void *)((uintptr_t)&stack_probe + ((uintptr_t)64 << 20));
  result = tealet_debug_swap_far(current, new_far, &old_far);
  assert(result == 0);

  result = tealet_switch(g_main, NULL, TEALET_XFER_DEFAULT);
  assert(result == TEALET_ERR_INVAL);

  result = tealet_debug_swap_far(current, old_far, &old_far);
  assert(result == 0);

  tealet_exit(g_main, NULL, TEALET_EXIT_DELETE);
  abort();
  return NULL;
}

void test_debug_swap_far_invalid_caller_check_child(void) {
  tealet_t *runner;

  /* Test purpose: ensure child-path caller validation is covered using
   * debug far-pointer mutation and proper restoration.
   */
  init_test();

  runner = tealet_new_native_call(g_main, test_invalid_caller_check_child_run, NULL, NULL);
  assert(runner != NULL);

  fini_test();
}
#endif

typedef struct test_entry_t {
  const char *name;
  void (*fn)(void);
} test_entry_t;

static test_entry_t test_list[] = {
    {"test_main_current", test_main_current},
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
