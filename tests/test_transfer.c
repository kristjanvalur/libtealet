#include <assert.h>
#include <stdlib.h>
#if !defined _MSC_VER || _MSC_VER >= 1600 /* VS2010 and above */
#include <stdint.h>
#endif

#include "tealet.h"
#include "tealet_extras.h"
#include "test_harness.h"

/* This file contains tests for transfer semantics, and ensures that status,
 * switch, exit, and argument-passing APIs behave consistently.
 */

tealet_t *test_status_run(tealet_t *t1, void *arg) {
  (void)arg;
  assert(t1 == tealet_current(t1));
  assert(!TEALET_IS_MAIN(t1));
  assert(tealet_status(t1) == TEALET_STATUS_ACTIVE);
  return g_main;
}

/* Verify that status and main/non-main identity invariants hold and do not
 * accidentally misclassify active tealets.
 */
void test_status(void) {
  tealet_t *stub1;
  int result;
  init_test();

  assert(tealet_status(g_main) == TEALET_STATUS_ACTIVE);
  assert(TEALET_IS_MAIN(g_main));

  stub1 = NULL;
  result = tealet_test_new_dispatch(g_main, &stub1, NULL, NULL, NULL);
  assert(result == 0);
  assert(stub1 != NULL);
  assert(tealet_status(stub1) == TEALET_STATUS_ACTIVE);
  assert(!TEALET_IS_MAIN(stub1));
  tealet_stub_run(stub1, test_status_run, NULL);
  tealet_delete(stub1);

  fini_test();
}

tealet_t *test_exit_run(tealet_t *t1, void *arg) {
  int result;
  assert(t1 != g_main);
  status += 1;
  result = tealet_exit(g_main, NULL, (int)(intptr_t)arg);
  abort();
  assert(result == 0);
  return (tealet_t *)-1;
}

/* Verify that exit behavior for default and delete modes is correct and does
 * not accidentally leave exited state inconsistent.
 */
void test_exit(void) {
  tealet_t *stub1, *stub2;
  int result;
  void *arg;
  init_test();
  stub1 = NULL;
  result = tealet_test_new_dispatch(g_main, &stub1, NULL, NULL, NULL);
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

static tealet_t *glob_t1;
static tealet_t *glob_t2;

tealet_t *test_switch_2(tealet_t *t2, void *arg) {
  (void)arg;
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
  (void)arg;
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

/* Verify that multi-hop switching preserves ordering and state progression and
 * does not accidentally corrupt switch sequencing.
 */
void test_switch(void) {
  init_test();
  assert(tealet_new_native_call(g_main, test_switch_1, NULL, NULL) != NULL);
  assert(status == 7);
  tealet_delete(glob_t1);
  tealet_delete(glob_t2);
  fini_test();
}

static tealet_t *switch_self_panic_runner(tealet_t *current, void *arg) {
  (void)arg;
  status = 1;
  return current->main;
}

/* Verify that PANIC self-switch is immediate and non-sticky, and does not
 * accidentally poison later unrelated switches.
 */
void test_switch_self_panic(void) {
  tealet_t *runner;
  int result;

  /* Verify that self-switch with PANIC returns TEALET_ERR_PANIC immediately
   * and does not accidentally leak panic state into later switches.
   */
  init_test();

  /* Self-switch with PANIC should consume panic immediately. */
  result = tealet_switch(g_main, NULL, TEALET_XFER_PANIC);
  assert(result == TEALET_ERR_PANIC);

  /* Ensure panic flag is not left armed for a later unrelated switch. */
  runner = tealet_new_native_call(g_main, switch_self_panic_runner, NULL, NULL);
  assert(runner != NULL);
  assert(status == 1);
  tealet_delete(runner);

  fini_test();
}

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

/* Verify that bidirectional argument passing across switch/exit is preserved
 * and does not accidentally drop updated payloads.
 */
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