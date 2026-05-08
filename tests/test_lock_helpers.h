#ifndef TEST_LOCK_HELPERS_H
#define TEST_LOCK_HELPERS_H

#include <assert.h>

#include "tealet.h"

/* Shared lock instrumentation for single-thread tests.
 *
 * We install a strict non-recursive fake lock callback pair and then assert:
 * - every lock has a matching unlock,
 * - lock depth returns to zero at test boundaries,
 * - tealet run bodies execute with no lock held.
 */
typedef struct tealet_test_lock_state_t {
  int depth;
  int lock_calls;
  int unlock_calls;
} tealet_test_lock_state_t;

static void tealet_test_lock_cb(void *arg) {
  tealet_test_lock_state_t *state = (tealet_test_lock_state_t *)arg;
  assert(state != NULL);
  assert(state->depth == 0);
  state->depth = 1;
  state->lock_calls++;
}

static void tealet_test_unlock_cb(void *arg) {
  tealet_test_lock_state_t *state = (tealet_test_lock_state_t *)arg;
  assert(state != NULL);
  assert(state->depth == 1);
  state->depth = 0;
  state->unlock_calls++;
}

static void tealet_test_lock_init(tealet_test_lock_state_t *state) {
  assert(state != NULL);
  state->depth = 0;
  state->lock_calls = 0;
  state->unlock_calls = 0;
}

static void tealet_test_lock_install(tealet_t *main_tealet, tealet_test_lock_state_t *state) {
  tealet_lock_t locking;
  int result;

  assert(main_tealet != NULL);
  tealet_test_lock_init(state);

  locking.mode = TEALET_LOCK_SWITCH;
  locking.lock = tealet_test_lock_cb;
  locking.unlock = tealet_test_unlock_cb;
  locking.arg = (void *)state;

  result = tealet_config_set_locking(main_tealet, &locking);
  assert(result == 0);
}

/* Test-level end-of-scope safety check: no lock leak and balanced calls. */
static void tealet_test_lock_assert_balanced(const tealet_test_lock_state_t *state) {
  assert(state != NULL);
  assert(state->depth == 0);
  assert(state->lock_calls == state->unlock_calls);
}

/* Run-path guard: tealet code itself should execute outside the lock. */
static void tealet_test_lock_assert_unheld(const tealet_test_lock_state_t *state) {
  assert(state != NULL);
  assert(state->depth == 0);
}

#endif
