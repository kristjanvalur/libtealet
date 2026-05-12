#include "test_lifecycle.h"

#include <assert.h>

#include "tealet_extras.h"
#include "test_harness.h"

/* This file contains tests for basic lifecycle flow, and ensures that create,
 * previous, switch, and exit APIs preserve expected state transitions.
 */

/* Verify that initialize/finalize preserves current-tealet invariants and
 * does not accidentally leave harness state dirty.
 */
void test_main_current(void) {
  init_test();
  fini_test();
}

/* Verify that tealet_set_far rejects non-main tealets and does not
 * accidentally return success for child handles.
 */
void test_set_far_non_main_invalid(void) {
  tealet_t *child;
  int result;
  char far_marker = 0;

  init_test();

  child = tealet_new(g_main);
  assert(child != NULL);

  result = tealet_set_far(child, &far_marker);
  assert(result == TEALET_ERR_INVAL);

  tealet_delete(child);
  fini_test();
}

/* Verify that unbound tealets reject switching and duplicate correctly, and do
 * not accidentally expose a runnable state.
 */
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

static tealet_t *test_simple_run(tealet_t *t1, void *arg) {
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

/* Verify that native create+run updates origin and previous fields and does
 * not accidentally lose lineage metadata.
 */
void test_simple(void) {
  tealet_t *t;
  init_test();
  t = tealet_new_native_call(g_main, test_simple_run, NULL, NULL);
  assert(t != NULL);
  assert(status == 1);
  tealet_delete(t);
  fini_test();
}

/* Verify that spawn default mode creates without immediate execution and does
 * not accidentally run user code early.
 */
void test_simple_create(void) {
  tealet_t *t;
  init_test();
  t = NULL;
  assert(tealet_spawn(g_main, &t, test_simple_run, NULL, NULL, TEALET_RUN_DEFAULT) == 0);
  assert(status == 0);
  tealet_delete(t);
  fini_test();
}

/* Verify that a spawned tealet runs on first explicit switch and does not
 * accidentally run more than once per handoff.
 */
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
  (void)arg;
  /* When first switched to via tealet_switch(), previous should be main */
  assert(tealet_previous(t1) == t1->main);
  status = 42;
  return g_main;
}

/* Verify that previous() inside first run points at main creator and does not
 * accidentally report stale previous state.
 */
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

/* Verify that deleting the previous tealet clears main->previous and does not
 * accidentally retain a dangling pointer.
 */
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

/* 1 is high on the stack.  We then create 2 lower on the stack */
/* the execution is : m 1 m 2 1 m 2 m */
static tealet_t *test_switch_new_1(tealet_t *t1, void *arg) {
  tealet_t *caller;
  tealet_t *stub;
  int result;
  caller = (tealet_t *)arg;
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

static tealet_t *test_switch_new_2(tealet_t *t2, void *arg) {
  tealet_t *target;
  target = (tealet_t *)arg;
  /* switch to tealet 1 to trample the stack*/
  target->extra = (void *)t2;
  tealet_switch(target, NULL, TEALET_XFER_DEFAULT);

  /* and then return to main */
  return g_main;
}

/* Verify that switch ordering remains correct across stack-depth changes and
 * does not accidentally corrupt transfer sequencing.
 */
void test_switch_new(void) {
  tealet_t *tealet1;
  tealet_t *tealet2;
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
