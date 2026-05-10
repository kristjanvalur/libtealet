/* Test tealet_fork functionality */

#include "tealet.h"
#include "test_lock_helpers.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int test_count = 0;
static int test_passed = 0;
static tealet_test_lock_state_t g_lock_state;

static void finalize_main_checked(tealet_t *main) {
  tealet_test_lock_assert_balanced(&g_lock_state);
  tealet_finalize(main);
}

static void assert_origin_main(tealet_t *t) {
  unsigned int origin;
  origin = tealet_get_origin(t);
  assert((origin & TEALET_ORIGIN_MAIN_LINEAGE) != 0);
  assert((origin & TEALET_ORIGIN_FORK) == 0);
}

static void assert_origin_main_fork(tealet_t *t) {
  unsigned int origin;
  origin = tealet_get_origin(t);
  assert((origin & TEALET_ORIGIN_MAIN_LINEAGE) != 0);
  assert((origin & TEALET_ORIGIN_FORK) != 0);
}

#define TEST(name)                                                                                                     \
  do {                                                                                                                 \
    printf("Running test: %s\n", name);                                                                                \
    test_count++;                                                                                                      \
  } while (0)

#define PASS()                                                                                                         \
  do {                                                                                                                 \
    printf("  PASSED\n");                                                                                              \
    test_passed++;                                                                                                     \
  } while (0)

static tealet_t *new_main_checked(void) {
  tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
  tealet_t *main;
  int result;

  main = tealet_initialize(&alloc, 0);
  assert(main != NULL);
  tealet_test_lock_install(main, &g_lock_state);
  result = tealet_configure_check_stack(main, 0);
  assert(result == 0);
  return main;
}

/* Test basic fork without TEALET_RUN_SWITCH */
static void test_basic_fork(void *far_marker) {
  tealet_t *main;
  tealet_t *other = NULL;
  int result;

  TEST("test_basic_fork");

  /* Initialize main tealet */
  main = new_main_checked();
  assert_origin_main(main);

  /* Set far boundary for main tealet */
  result = tealet_set_far(main, far_marker);
  assert(result == 0);

  int testvalue = 0;

  other = tealet_new(main);
  assert(other != NULL);

  /* Fork - creates child but stays in parent */
  result = tealet_fork(other, &other, NULL, TEALET_RUN_DEFAULT);

  if (result == 1) {
    /* We are the parent, other = child */
    assert(other != NULL);
    assert_origin_main(main);
    assert_origin_main_fork(other);
    assert(testvalue == 0); /* Parent should have saved value */
    printf("  Parent: switching to child...\n");

    /* Switch to child */
    result = tealet_switch(other, NULL, TEALET_SWITCH_DEFAULT);
    assert(result == 0);
    assert(testvalue == 0); /* Parent value should be preserved after child modifies it */

    /* Verify tealet_previous() after switch back from child
     * The child (stored in 'other') called tealet_exit() back to us */
    tealet_t *prev = tealet_previous(main);
    printf("  Parent: previous tealet is %p, child is %p\n", (void *)prev, (void *)other);
    assert(prev == other);
    printf("  Parent: returned from child, stack preserved correctly\n");
    printf("  Parent: tealet_previous() verified after child exit\n");

    /* Clean up */
    tealet_delete(other);
    finalize_main_checked(main);

    PASS();
  } else if (result == 0) {
    /* We are the child, other = parent */
    tealet_test_lock_assert_unheld(&g_lock_state);
    assert_origin_main_fork(tealet_current(main));
    assert_origin_main(other);
    assert(testvalue == 0); /* Child should start with saved value */

    /* Verify tealet_previous() in child after switch from parent
     * The parent (stored in 'other') called tealet_switch() to us */
    tealet_t *prev = tealet_previous(main);
    printf("  Child: previous tealet is %p, parent is %p\n", (void *)prev, (void *)other);
    assert(prev == other);
    printf("  Child: tealet_previous() verified after switch from parent\n");

    /* Modify stack variable to verify parent's stack is isolated */
    testvalue = 42;
    printf("  Child: exiting via tealet_exit...\n");

    /* Child must explicitly exit - no run function to return from */
    tealet_exit(other, NULL, 0);

    printf("  Child: this should never print\n");
    assert(0);
  } else {
    /* Error */
    printf("  Error: fork returned %d\n", result);
    assert(0);
  }
}

/* Test fork with TEALET_RUN_SWITCH */
static void test_fork_switch(void *far_marker) {
  tealet_t *main;
  tealet_t *other = NULL;
  int result;
  int switch_count = 0;

  TEST("test_fork_switch");

  /* Initialize main tealet */
  main = new_main_checked();
  assert_origin_main(main);

  /* Set far boundary for main tealet */
  result = tealet_set_far(main, far_marker);
  assert(result == 0);

  printf("  Before fork: switch_count=%d\n", switch_count);
  int testvalue = 0;

  other = tealet_new(main);
  assert(other != NULL);

  /* Fork with immediate switch - becomes child immediately */
  result = tealet_fork(other, &other, NULL, TEALET_RUN_SWITCH);

  switch_count++;

  if (result == 0) {
    /* We are the child */
    tealet_test_lock_assert_unheld(&g_lock_state);
    assert_origin_main_fork(tealet_current(main));
    assert_origin_main(other);
    assert(testvalue == 0);    /* Child should start with saved value */
    assert(switch_count == 1); /* Child should have incremented value */

    /* Verify tealet_previous() in child - should be parent after fork with
     * FORK_SWITCH */
    assert(tealet_previous(main) == other);
    printf("  Child: tealet_previous() verified after FORK_SWITCH\n");

    /* Modify stack variables to verify parent's stack is isolated */
    testvalue = 42;
    printf("  Child: exiting via tealet_exit...\n");

    /* Child must explicitly exit - no run function to return from */
    tealet_exit(other, NULL, 0);
    printf("  Child: this should never print\n");
    assert(0);
  } else {
    /* We are the parent, switched back to */
    assert(result == 1);
    assert_origin_main(main);
    assert_origin_main_fork(other);
    assert(testvalue == 0);    /* Parent should have saved value, not child's 42 */
    assert(switch_count == 1); /* Parent should have saved value, not child's 2 */

    /* Verify tealet_previous() after child exits back to parent */
    assert(tealet_previous(main) == other);
    printf("  Parent: tealet_previous() verified after child exit\n");
    printf("  Parent: stack variables preserved correctly\n");
    printf("  Parent: back from child, cleaning up\n");
  } /* Clean up - only parent gets here */
  tealet_delete(other);
  finalize_main_checked(main);

  PASS();
}

/* Test multiple forks */
static void test_multiple_forks(void *far_marker) {
  tealet_t *main;
  tealet_t *child1 = NULL;
  tealet_t *child2 = NULL;
  int result;
  int visited = 0;

  TEST("test_multiple_forks");

  /* Initialize main tealet */
  main = new_main_checked();
  assert_origin_main(main);

  /* Set far boundary for main tealet */
  result = tealet_set_far(main, far_marker);
  assert(result == 0);

  /* Create first child */
  child1 = tealet_new(main);
  assert(child1 != NULL);
  result = tealet_fork(child1, &child1, NULL, TEALET_RUN_DEFAULT);
  if (result == 0) {
    /* We are child1 */
    printf("  Child1: woke up, exiting\n");
    tealet_exit(child1, NULL, 0);
    assert(0);
  }
  assert(child1 != NULL);
  assert_origin_main_fork(child1);
  printf("  Parent: created child1=%p\n", (void *)child1);

  /* Create second child */
  child2 = tealet_new(main);
  assert(child2 != NULL);
  result = tealet_fork(child2, &child2, NULL, TEALET_RUN_DEFAULT);
  if (result == 0) {
    /* We are child2 */
    printf("  Child2: woke up, exiting\n");
    tealet_exit(child2, NULL, 0);
    assert(0);
  }
  assert(child2 != NULL);
  assert_origin_main_fork(child2);
  printf("  Parent: created child2=%p\n", (void *)child2);

  /* Switch to child1 */
  visited++;
  printf("  Parent: switching to child1 (visited=%d)\n", visited);
  tealet_switch(child1, NULL, TEALET_SWITCH_DEFAULT);
  printf("  Parent: returned from child1\n");

  /* Switch to child2 */
  visited++;
  printf("  Parent: switching to child2 (visited=%d)\n", visited);
  tealet_switch(child2, NULL, TEALET_SWITCH_DEFAULT);
  printf("  Parent: returned from child2\n");

  assert(visited == 2);

  /* Clean up */
  tealet_delete(child1);
  tealet_delete(child2);
  finalize_main_checked(main);

  PASS();
}

/* Test fork argument passing with TEALET_RUN_SWITCH */
static void test_fork_switch_arg(void *far_marker) {
  tealet_t *main;
  tealet_t *other = NULL;
  int result;
  void *arg = NULL;

  TEST("test_fork_switch_arg");

  /* Initialize main tealet */
  main = new_main_checked();
  assert_origin_main(main);

  /* Set far boundary for main tealet */
  result = tealet_set_far(main, far_marker);
  assert(result == 0);

  /* Fork with FORK_SWITCH - parent passes parg, child switches back with value
   */
  other = tealet_new(main);
  assert(other != NULL);
  result = tealet_fork(other, &other, &arg, TEALET_RUN_SWITCH);

  if (result == 0) {
    /* We are the child - arg should be NULL initially */
    void *childarg;
    assert_origin_main_fork(tealet_current(main));
    assert_origin_main(other);
    assert(arg == NULL);

    /* Switch back to parent with a value */
    childarg = (void *)0x12345678;
    printf("  Child: started.switching back with value %p\n", childarg);
    tealet_switch(other, &childarg, TEALET_SWITCH_DEFAULT);
    /* parent switched back, arg should be updated */
    assert(childarg == (void *)0xdeadbeef);
    printf("  Child: switched back from parent with arg=%p\n", childarg);
    /* final exit to parent*/
    childarg = (void *)0xfeedbeef;
    printf("  Child: exiting to parent with arg=%p\n", childarg);
    tealet_exit(other, childarg, 0);
    printf("  Child: this should never print\n");
    assert(0);
  } else {
    /* We are the parent - child should have passed back a value */
    void *parentarg;
    assert(result == 1);
    assert_origin_main(main);
    assert_origin_main_fork(other);
    printf("  Parent: received arg=%p from child\n", arg);
    assert(arg == (void *)0x12345678);
    parentarg = (void *)0xdeadbeef;
    /* switch back to child with a different arg, for the child to exit*/
    printf("  Parent: switching back to child with arg=%p\n", parentarg);
    result = tealet_switch(other, &parentarg, TEALET_SWITCH_DEFAULT);
    printf("  Parent: returned from child switch, arg=%p, result=%d\n", parentarg, result);
    /* child should have exited, and passed the final arg to us*/
    assert(result == 0);
    assert(parentarg == (void *)0xfeedbeef);
  }

  /* Clean up - only parent gets here */
  tealet_delete(other);
  finalize_main_checked(main);

  PASS();
}

/* Test fork argument passing with TEALET_RUN_DEFAULT */
static void test_fork_default_arg(void *far_marker) {
  tealet_t *main;
  tealet_t *child = NULL;
  int result;
  void *arg = NULL;

  TEST("test_fork_default_arg");

  /* Initialize main tealet */
  main = new_main_checked();
  assert_origin_main(main);

  /* Set far boundary for main tealet */
  result = tealet_set_far(main, far_marker);
  assert(result == 0);

  /* Fork without FORK_SWITCH - stays in parent */
  child = tealet_new(main);
  assert(child != NULL);
  result = tealet_fork(child, &child, &arg, TEALET_RUN_DEFAULT);

  if (result == 1) {
    /* We are the parent - arg should still be NULL */
    void *parentarg;
    assert(arg == NULL);
    assert(child != NULL);
    assert_origin_main_fork(child);
    printf("  Parent: fork returned, arg=%p, switching to child with value\n", arg);

    /* Switch to child with a value */
    parentarg = (void *)0xABCDEF00;
    result = tealet_switch(child, &parentarg, TEALET_SWITCH_DEFAULT);
    assert(result == 0);

    /* Child should have switched back with a different value */
    printf("  Parent: child switched back, arg=%p\n", parentarg);
    assert(parentarg == (void *)0xDEADBEEF);
    printf("  Parent: bidirectional argument passing verified\n");

    /* Clean up */
    tealet_delete(child);
    finalize_main_checked(main);

    PASS();
  } else if (result == 0) {
    /* We are the child - arg should have the value parent passed */
    void *childarg;
    assert_origin_main_fork(tealet_current(main));
    assert_origin_main(child);
    printf("  Child: woke up with arg=%p\n", arg);
    assert(arg == (void *)0xABCDEF00);

    /* Switch back to parent with a different value */
    childarg = (void *)0xDEADBEEF;
    tealet_switch(child, &childarg, TEALET_SWITCH_DEFAULT); /* child pointer is parent from child's perspective */

    printf("  Child: this should never print\n");
    assert(0);
  } else {
    /* Error */
    printf("  Error: fork returned %d\n", result);
    assert(0);
  }
}

/* Test fork then switch back and forth */
static void test_ping_pong(void *far_marker) {
  tealet_t *main;
  tealet_t *child = NULL;
  int result;
  int counter = 0;

  TEST("test_ping_pong");

  /* Initialize main tealet */
  main = new_main_checked();
  assert_origin_main(main);

  /* Set far boundary for main tealet */
  result = tealet_set_far(main, far_marker);
  assert(result == 0);

  /* Array to verify stack isolation - won't be in registers */
  int data[5] = {0, 0, 0, 0, 0};

  /* Fork */
  child = tealet_new(main);
  assert(child != NULL);
  result = tealet_fork(child, &child, NULL, TEALET_RUN_DEFAULT);

  counter++;

  if (result == 1) {
    /* Parent */
    tealet_t *child_saved = child; /* Save - local vars on swapped stack */
    assert_origin_main(child_saved->main);
    assert_origin_main_fork(child_saved);
    printf("  Parent: counter=%d, data=[%d,%d,%d,%d,%d], switching to child\n", counter, data[0], data[1], data[2],
           data[3], data[4]);

    /* Ping-pong a few times */
    while (counter <= 5) {
      tealet_switch(child_saved, NULL, TEALET_SWITCH_DEFAULT);
      counter++;
      /* Parent increments by 1 each iteration (but only first 4) */
      if (counter <= 5) {
        data[counter - 2]++;
      }
      /* Verify parent's data array is isolated from child modifications */
      printf("  Parent: counter=%d, data=[%d,%d,%d,%d,%d]\n", counter, data[0], data[1], data[2], data[3], data[4]);
    }

    /* Verify final parent data: should be [1,1,1,1,0] */
    assert(data[0] == 1 && data[1] == 1 && data[2] == 1 && data[3] == 1 && data[4] == 0);
    printf("  Parent: data verified as private (correct)\n");
    printf("  Parent: done, cleaning up\n");

    /* Clean up */
    tealet_delete(child_saved);
    finalize_main_checked(main);

    PASS();
  } else {
    /* Child */
    assert(result == 0);
    assert_origin_main_fork(tealet_current(main));
    assert_origin_main(child);
    printf("  Child: counter=%d, data=[%d,%d,%d,%d,%d], switching to parent\n", counter, data[0], data[1], data[2],
           data[3], data[4]);

    while (counter < 5) {
      /* Child increments by 10 each iteration (counter goes 1,2,3,4 -> indices
       * 0,1,2,3) */
      data[counter - 1] += 10;
      tealet_switch(child, NULL, TEALET_SWITCH_DEFAULT); /* child pointer is parent from child's perspective */
      counter++;
      printf("  Child: counter=%d, data=[%d,%d,%d,%d,%d], switching to parent\n", counter, data[0], data[1], data[2],
             data[3], data[4]);
    }

    /* Verify final child data: should be [10,10,10,10,0] */
    assert(data[0] == 10 && data[1] == 10 && data[2] == 10 && data[3] == 10 && data[4] == 0);
    printf("  Child: data verified as private (correct)\n");

    /* Final switch back to parent for cleanup */
    printf("  Child: exiting\n");
    tealet_exit(child, NULL, 0);
  }
}

/* Test tealet_previous() with tealet_new() */
static tealet_t *test_new_previous_run(tealet_t *current, void *arg) {
  tealet_t *main = current->main;
  tealet_t *expected_caller = (tealet_t *)arg;

  /* Verify tealet_previous() returns the tealet that called tealet_new() */
  assert(tealet_previous(main) == expected_caller);
  printf("  Run function: tealet_previous() verified - caller is %s\n",
         expected_caller == main ? "main" : "other tealet");

  return main;
}

static void test_new_previous(void *far_marker) {
  tealet_t *main;
  tealet_t *started;
  void *arg;

  TEST("test_new_previous");

  /* Initialize main tealet */
  main = new_main_checked();

  /* Test tealet_new()+tealet_run(..., SWITCH) from main */
  arg = main;
  started = tealet_new(main);
  assert(started != NULL);
  assert(tealet_run(started, test_new_previous_run, &arg, NULL, TEALET_RUN_SWITCH) == 0);

  /* Verify tealet_previous() after return from tealet_run() */
  /* Note: the tealet has already been freed at this point, so we just verify
   * we're back in main */
  assert(tealet_current(main) == main);
  printf("  Main: returned from tealet_run(SWITCH), tealet_previous() test passed\n");

  finalize_main_checked(main);

  PASS();
}

int main(void) {
  /* Disable stdout buffering to see debug output before crashes */
  setbuf(stdout, NULL);
  int far_marker; /* marker for far boundary */

  printf("=== Testing tealet_fork ===\n\n");

  test_basic_fork(&far_marker);
  printf("\n");

  test_fork_switch(&far_marker);
  printf("\n");

  test_fork_switch_arg(&far_marker);
  printf("\n");

  test_fork_default_arg(&far_marker);
  printf("\n");

  test_multiple_forks(&far_marker);
  printf("\n");

  test_ping_pong(&far_marker);
  printf("\n");

  test_new_previous(&far_marker);
  printf("\n");

  printf("=== Results: %d/%d tests passed ===\n", test_passed, test_count);

  return (test_passed == test_count) ? 0 : 1;
}
