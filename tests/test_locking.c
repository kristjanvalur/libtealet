#include <assert.h>
#include <stdlib.h>

#include "tealet.h"
#include "tealet_extras.h"
#include "test_harness.h"

/* This file contains tests for lock transition accounting, and ensures that
 * create/switch/exit APIs trigger balanced lock and unlock callbacks.
 */

typedef enum lock_transition_phase_e {
  LOCK_PHASE_NONE = 0,
  LOCK_PHASE_NEW_START = 1,
  LOCK_PHASE_WAIT_RESUME = 2,
  LOCK_PHASE_SWITCH_RESUME = 3,
  LOCK_PHASE_STUB_RUN_START = 4,
  LOCK_PHASE_DONE = 5,
} lock_transition_phase_t;

static lock_transition_phase_t g_lock_phase = LOCK_PHASE_NONE;
static lock_snapshot_t g_lock_new_before;
static lock_snapshot_t g_lock_switch_before;
static lock_snapshot_t g_lock_exit_before;
static lock_snapshot_t g_lock_stub_new_before;
static lock_snapshot_t g_lock_stub_run_before;
static lock_snapshot_t g_lock_fork_before;

/* Transition accounting test for tealet_new + switch + exit.
 * Verifies expected lock/unlock deltas at each phase boundary.
 */
static tealet_t *test_lock_transition_run(tealet_t *current, void *arg) {
  (void)current;
  (void)arg;

  test_lock_assert_unheld();
  assert(g_lock_phase == LOCK_PHASE_NEW_START);
  lock_snapshot_assert_delta_one(&g_lock_new_before);

  g_lock_phase = LOCK_PHASE_WAIT_RESUME;
  tealet_switch(g_main, NULL, TEALET_XFER_DEFAULT);

  test_lock_assert_unheld();
  assert(g_lock_phase == LOCK_PHASE_SWITCH_RESUME);
  lock_snapshot_assert_delta_one(&g_lock_switch_before);

  lock_snapshot_take(&g_lock_exit_before);
  g_lock_phase = LOCK_PHASE_DONE;
  tealet_exit(g_main, NULL, TEALET_EXIT_DELETE);
  abort();
  return NULL;
}

/* Verify that direct create/switch/exit transitions produce balanced lock
 * callbacks and do not accidentally leave the lock held.
 */
void test_lock_transitions(void) {
  tealet_t *t;
  int result;

  init_test();
  init_test_locking();

  g_lock_phase = LOCK_PHASE_NEW_START;
  lock_snapshot_take(&g_lock_new_before);
  t = tealet_new_native_call(g_main, test_lock_transition_run, NULL, NULL);
  assert(t != NULL);
  assert(g_lock_phase == LOCK_PHASE_WAIT_RESUME);

  lock_snapshot_take(&g_lock_switch_before);
  g_lock_phase = LOCK_PHASE_SWITCH_RESUME;
  result = tealet_switch(t, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);
  assert(g_lock_phase == LOCK_PHASE_DONE);

  lock_snapshot_assert_delta_one(&g_lock_exit_before);

  fini_test();
}

/* Transition accounting helper for tealet_stub_run() path. */
static tealet_t *test_lock_transition_stub_run(tealet_t *current, void *arg) {
  (void)current;
  (void)arg;

  test_lock_assert_unheld();
  assert(g_lock_phase == LOCK_PHASE_STUB_RUN_START);
  lock_snapshot_assert_delta_one(&g_lock_stub_run_before);

  lock_snapshot_take(&g_lock_exit_before);
  g_lock_phase = LOCK_PHASE_DONE;
  tealet_exit(g_main, NULL, TEALET_EXIT_DELETE);
  abort();
  return NULL;
}

/* Verify that stub creation and first stub run produce balanced lock
 * callbacks and do not accidentally skip unlock on exit.
 */
void test_lock_transitions_stub(void) {
  tealet_t *stub;
  int result;

  init_test();
  init_test_locking();

  lock_snapshot_take(&g_lock_stub_new_before);
  result = tealet_stub_new(g_main, &stub, NULL);
  assert(result == 0);
  assert(stub != NULL);
  lock_snapshot_assert_delta_one(&g_lock_stub_new_before);

  lock_snapshot_take(&g_lock_stub_run_before);
  g_lock_phase = LOCK_PHASE_STUB_RUN_START;
  result = tealet_stub_run(stub, test_lock_transition_stub_run, NULL);
  assert(result == 0);
  assert(g_lock_phase == LOCK_PHASE_DONE);

  lock_snapshot_assert_delta_one(&g_lock_exit_before);

  fini_test();
}

/* Verify that tealet_fork() performs one balanced lock transition and does
 * not accidentally continue child execution after exit.
 */
void test_lock_transitions_fork(void) {
  tealet_t *other = NULL;
  int result;
  int is_child;
  char far_marker = 0;

  init_test();
  init_test_locking();

  result = tealet_set_far(g_main, &far_marker);
  assert(result == 0);

  other = tealet_new(g_main);
  assert(other != NULL);

  lock_snapshot_take(&g_lock_fork_before);
  result = tealet_fork(other, NULL, TEALET_START_DEFAULT);
  assert(result == 0);
  lock_snapshot_assert_delta_one(&g_lock_fork_before);

  is_child = (tealet_current(other) == other);

  if (!is_child) {
    assert(other != NULL);
    tealet_delete(other);
    fini_test();
    return;
  }

  /* If we execute as child, this path should not continue after exit. */
  assert(is_child);
  other = tealet_previous(g_main);
  test_lock_assert_unheld();
  tealet_exit(other, NULL, 0);
  abort();
}
