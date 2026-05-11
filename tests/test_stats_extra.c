#include "test_stats_extra.h"

#include <assert.h>
#include <string.h>

#include "tealet_extras.h"
#include "test_harness.h"

/* This file contains tests for extras payload propagation and stats
 * accounting, and ensures that related APIs report consistent allocation and
 * counter behavior.
 */

typedef struct extradata {
  int foo;
  char bar[5];
  int gaz;
} extradata;

static tealet_t *extra_tealet(tealet_t *cur, void *arg) {
  extradata ed2 = {1, "abcd", 2};
  extradata *ed1 = TEALET_EXTRA(cur, extradata);
  (void)arg;
  assert(ed1->foo == ed2.foo);
  assert(strcmp(ed1->bar, ed2.bar) == 0);
  assert(ed1->gaz == ed2.gaz);
  return g_main;
}

/* Verify that TEALET_EXTRA payload survives duplicate and stub-run paths and
 * does not accidentally lose field values.
 */
void test_extra(void) {
  tealet_t *t1;
  tealet_t *t2;
  extradata ed = {1, "abcd", 2};
  int result;
  init_test_extra(NULL, sizeof(extradata));
  *TEALET_EXTRA(g_main, extradata) = ed;

  t1 = NULL;
  result = tealet_test_new_dispatch(g_main, &t1, NULL, NULL, NULL);
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

/* Verify that the stats allocator wrapper records initialization allocations
 * and does not accidentally undercount allocation activity.
 */
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

/* Verify that active/total stats counters track create/delete transitions and
 * do not accidentally drift after teardown.
 */
void test_stats(void) {
  tealet_t *t1;
  tealet_stats_t stats;
  int a;
  int b;
  int result;
  init_test_extra(NULL, 0);

  /* Skip this test if stats are not enabled */
  tealet_get_stats(g_main, &stats);
  if (stats.blocks_allocated == 0) {
    fini_test();
    return;
  }

  assert(stats.n_active == 1);
  assert(stats.n_total == 1);
  t1 = NULL;
  result = tealet_test_new_dispatch(g_main, &t1, NULL, NULL, NULL);
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
