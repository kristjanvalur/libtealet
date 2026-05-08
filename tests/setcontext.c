/*
 *  * This test is an adaption of the example for the setcontext() library
 *   * as illustrated at https://en.wikipedia.org/wiki/Setcontext
 *    * It serves to show how to do a similar thin using tealets
 *     */

#include "tealet.h"
#include "tealet_extras.h"
#include "test_lock_helpers.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static tealet_test_lock_state_t g_lock_state;

/* This is the iterator function. It is entered on the first call to
 * swapcontext, and loops from 0 to 9. Each value is saved in i_from_iterator,
 * and then swapcontext used to return to the main loop.  The main loop prints
 * the value and calls swapcontext to swap back into the function. When the end
 * of the loop is reached, the function exits, and execution switches to the
 * context pointed to by main_context1. */
tealet_t *loop_func(tealet_t *current, void *arg) {
  int i;

  tealet_test_lock_assert_unheld(&g_lock_state);

  for (i = 0; i < (int)(intptr_t)arg; ++i) {
    /* Write the loop counter a variable used for passing between tealets. */
    void *value = (void *)(intptr_t)i;

    /* Switch to main tealet */
    tealet_switch(tealet_previous(current), &value, TEALET_SWITCH_DEFAULT);
    tealet_test_lock_assert_unheld(&g_lock_state);
    /* after switch, any void* passed _to_ us is in 'value' */
  }
  /* exit, without deleting, so that the caller can query status */
  tealet_exit(tealet_previous(current), NULL, TEALET_EXIT_DEFAULT);
  return 0; /* not reached */
}

int main(void) {
  /* initialize main tealet using malloc based allocation */
  tealet_alloc_t talloc = TEALET_ALLOC_INIT_MALLOC;
  tealet_t *tmain = tealet_initialize(&talloc, 0);
  tealet_t *loop;
  void *data; /* data exchange object */
  int configure_result;

  if (tmain == NULL)
    return 1;

  tealet_test_lock_install(tmain, &g_lock_state);

  configure_result = tealet_configure_check_stack(tmain, 0);
  if (configure_result != 0) {
    tealet_test_lock_assert_balanced(&g_lock_state);
    tealet_finalize(tmain);
    return 1;
  }

  /* how many rounds? */
  data = (void *)10;
  loop = NULL;
  if (tealet_new(tmain, &loop, loop_func, &data, NULL) != 0) {
    tealet_test_lock_assert_balanced(&g_lock_state);
    tealet_finalize(tmain);
    return 1;
  }

  /* loop until the tealet has exited */
  while (tealet_status(loop) == TEALET_STATUS_ACTIVE) {
    /* we don't pass anything _in_ to the loop here,
     * only retrieve the result
     */
    printf("%d\n", (int)(intptr_t)data);
    tealet_switch(loop, &data, TEALET_SWITCH_DEFAULT);
  }
  tealet_delete(loop);
  tealet_test_lock_assert_balanced(&g_lock_state);
  tealet_finalize(tmain);
  return 0;
}
