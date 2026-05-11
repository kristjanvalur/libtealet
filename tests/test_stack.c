#include "test_stack.h"

#include <assert.h>

#include "test_harness.h"

/* This file contains tests for stack-distance helpers and stack-far
 * isolation, and ensures that stack-related APIs keep parent/child data
 * separated.
 */

typedef struct stack_far_case_t {
  int value;
} stack_far_case_t;

typedef struct stack_far_run_arg_t {
  tealet_t *main;
  stack_far_case_t *shared;
  int before;
  int after;
} stack_far_run_arg_t;

static void test_stack_further_inner(void *outer_addr) {
  int inner_local;
  void *further;
  further = tealet_stack_further(outer_addr, &inner_local);
  assert(further == outer_addr);
}

static tealet_t *test_stack_far_isolation_run(tealet_t *current, void *arg) {
  stack_far_run_arg_t *run_arg;
  run_arg = (stack_far_run_arg_t *)arg;
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
  child = tealet_new_native_call(current, test_stack_far_isolation_run, &child_arg, stack_far);
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

/* Verify that tealet_stack_further chooses consistent farther addresses and
 * does not accidentally invert stack-distance ordering.
 */
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

/* Verify that extended stack_far creation preserves parent/child isolation and
 * does not accidentally leak child writes into parent storage.
 */
void test_stack_far_isolation(void) {
  tealet_t *parent;

  init_test();
  parent = tealet_new_native_call(g_main, test_stack_far_isolation_parent, NULL, NULL);
  assert(parent != NULL);
  tealet_delete(parent);
  fini_test();
}
