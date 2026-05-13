/*
 * This test follows the control-flow shape of the setcontext/swapcontext
 * iterator example from:
 * https://en.wikipedia.org/wiki/Setcontext#Example
 *
 * Mapping of the original idea to tealets:
 * - iterator context function      -> loop_func()
 * - swapcontext(main, iterator)    -> tealet_run(..., TEALET_START_SWITCH)
 *   and later tealet_switch(loop, ...)
 * - swapcontext(iterator, main)    -> tealet_switch(tealet_previous(current), ...)
 * - iterator completion            -> return tealet_previous(current)
 *
 * As in the original example, the iterator yields one value per iteration,
 * main prints it, and then resumes the iterator until completion.
 *
 * This file also exercises the draft examples/setcontext.h wrapper API
 * (`tealetex_*`) to validate a setcontext-like facade built over libtealet.
 */

#include "setcontext.h"
#include "tealet.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Iterator function analogous to the setcontext example's iterator context.
 * It yields values to main and resumes on each switch back until it exits.
 */
tealet_t *loop_func(tealet_t *current, void *arg) {
  int i;

  for (i = 0; i < (int)(intptr_t)arg; ++i) {
    /* Write the loop counter a variable used for passing between tealets. */
    void *value = (void *)(intptr_t)i;

    /* Switch to main tealet */
    tealet_switch(tealet_previous(current), &value, TEALET_XFER_DEFAULT);
    /* after switch, any void* passed _to_ us is in 'value' */
  }
  /* return-flow completion: hand control back to previous tealet */
  return tealet_previous(current);
}

static int run_direct_example(void) {
  /* initialize main tealet using malloc based allocation */
  tealet_alloc_t talloc = TEALET_ALLOC_INIT_MALLOC;
  tealet_t *tmain = tealet_initialize(&talloc, 0);
  tealet_t *loop;
  void *data; /* data exchange object */

  if (tmain == NULL)
    return 1;

  /* how many rounds? */
  data = (void *)10;
  loop = tealet_new(tmain);
  if (loop == NULL) {
    tealet_finalize(tmain);
    return 1;
  }
  if (tealet_run(loop, loop_func, &data, NULL, TEALET_START_SWITCH) != 0) {
    tealet_delete(loop);
    tealet_finalize(tmain);
    return 1;
  }

  /* loop until the tealet has exited */
  while (tealet_status(loop) == TEALET_STATUS_ACTIVE) {
    /* we don't pass anything _in_ to the loop here,
     * only retrieve the result
     */
    printf("%d\n", (int)(intptr_t)data);
    tealet_switch(loop, &data, TEALET_XFER_DEFAULT);
  }
  tealet_delete(loop);
  tealet_finalize(tmain);
  return 0;
}

static int run_tealetex_wrapper_example(void) {
  tealetex_setcontext_main_t scmain;
  tealetex_ucontext_t main_uc;
  tealetex_ucontext_t loop_uc;
  void *data;
  int expected;
  int loop_uc_ready;
  int result;

  loop_uc_ready = 0;

  result = tealetex_getcontext_init(&scmain);
  if (result != 0)
    return 1;

  result = tealetex_getcontext(&scmain, &main_uc);
  if (result != 0)
    goto fail;

  result = tealetex_getcontext(&scmain, &loop_uc);
  if (result != 0)
    goto fail;
  loop_uc_ready = 1;

  loop_uc.uc_link = &main_uc;
  result = tealetex_makecontext(&scmain, &loop_uc, loop_func, (void *)10, NULL, TEALET_START_SWITCH);
  if (result != 0)
    goto fail;

  data = NULL;
  result = tealetex_setcontext(&scmain, &loop_uc, &data);
  if (result != 0)
    goto fail;

  expected = 0;
  while (tealet_status(loop_uc.uc_tealet) == TEALET_STATUS_ACTIVE) {
    if ((int)(intptr_t)data != expected)
      goto fail;
    expected += 1;

    result = tealetex_setcontext(&scmain, &loop_uc, &data);
    if (result != 0)
      goto fail;
  }

  if (expected != 10)
    goto fail;

  if (loop_uc_ready)
    tealetex_freecontext(&scmain, &loop_uc);
  tealetex_getcontext_fini(&scmain);
  return 0;

fail:
  if (loop_uc_ready)
    tealetex_freecontext(&scmain, &loop_uc);
  tealetex_getcontext_fini(&scmain);
  return 1;
}

int main(void) {
  if (run_direct_example() != 0)
    return 1;
  if (run_tealetex_wrapper_example() != 0)
    return 1;
  return 0;
}
