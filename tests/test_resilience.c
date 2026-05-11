#include "test_resilience.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "tealet_extras.h"
#include "test_harness.h"

#if TEALET_WITH_TESTING
int tealet_debug_force_defunct(tealet_t *target);
int tealet_debug_swap_far(tealet_t *target, void *new_far, void **old_far);
#endif

tealet_t *mem_error_tealet(tealet_t *t1, void *arg) {
  void *myarg;
  int res;
  tealet_t *peer = (tealet_t *)arg;
  talloc_fail = 1;
  res = tealet_switch(peer, &myarg, TEALET_XFER_DEFAULT);
  assert(res == TEALET_ERR_MEM);
  tealet_exit(peer, myarg, TEALET_EXIT_DELETE);
  abort(); /* never runs */
  (void)t1;
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
  assert(tealet_spawn(g_main, &worker, oom_force_to_main_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &worker, oom_main_probe_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &oom_w2, oom_force_peer_to_main_panic_run, NULL,
                      NULL, TEALET_RUN_DEFAULT) == 0);
  assert(oom_w2 != NULL);

  w1 = NULL;
  assert(tealet_spawn(g_main, &w1, oom_force_to_peer_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &worker, switch_nofail_mem_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &victim, oom_force_to_main_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
  assert(victim != NULL);

  result = tealet_switch(victim, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);
  talloc_fail = 0;
  assert(tealet_status(victim) == TEALET_STATUS_DEFUNCT);

  switcher = NULL;
  assert(tealet_spawn(g_main, &switcher, switch_nofail_defunct_target_run, NULL,
                      NULL, TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &worker, exit_nofail_mem_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &victim, oom_force_to_main_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
  assert(victim != NULL);

  result = tealet_switch(victim, NULL, TEALET_XFER_DEFAULT);
  assert(result == 0);
  talloc_fail = 0;
  assert(tealet_status(victim) == TEALET_STATUS_DEFUNCT);

  exiter = NULL;
  assert(tealet_spawn(g_main, &exiter, exit_nofail_defunct_target_run, NULL,
                      NULL, TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &runner, test_exit_self_invalid_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &exiter, test_exit_defunct_fail_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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
  assert(tealet_spawn(g_main, &exiter, test_explicit_panic_exit_run, NULL, NULL,
                      TEALET_RUN_DEFAULT) == 0);
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

static tealet_t *test_invalid_caller_check_child_run(tealet_t *current,
                                                      void *arg) {
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

  runner = tealet_new_native_call(g_main, test_invalid_caller_check_child_run,
                                  NULL, NULL);
  assert(runner != NULL);

  fini_test();
}
#endif
