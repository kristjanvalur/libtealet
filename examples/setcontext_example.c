/*
 * setcontext/swapcontext iterator example translated to tealetex_* APIs.
 *
 * Source model:
 * https://en.wikipedia.org/wiki/Setcontext#Example
 */

#include "setcontext.h"

#include <stdint.h>
#include <stdio.h>

/* The setcontext domain for this example. */
static tealetex_setcontext_main_t g_scmain;

/* The three contexts:
 *   (1) main_context1: return target when loop completes (uc_link).
 *   (2) main_context2: point in main used by swapcontext-like transfers.
 *   (3) loop_context : the iterator context.
 */
static tealetex_ucontext_t main_context1;
static tealetex_ucontext_t main_context2;
static tealetex_ucontext_t loop_context;

/* Iterator return value. */
static volatile int i_from_iterator;

/* Iterator function. It yields values and switches back to main_context2.
 * When it returns, control flows to loop_context.uc_link (main_context1).
 */
static void loop(tealetex_ucontext_t *loop_context, tealetex_ucontext_t *other_context,
                 int *i_from_iterator_ptr) {
  int i;

  if (loop_context == NULL || other_context == NULL || i_from_iterator_ptr == NULL)
    return;

  for (i = 0; i < 10; ++i) {
    *i_from_iterator_ptr = i;

    tealetex_swapcontext(&g_scmain, loop_context, other_context);
  }

  /* The wrapper in examples/setcontext.c applies implicit uc_link transfer
   * when this function returns, like canonical setcontext examples.
   */
}

int main(int argc, char **argv) {
  int iterator_finished;

  (void)argc;

  tealetex_getcontext_init(&g_scmain, (void *)&argv);
  tealetex_getcontext(&g_scmain, &loop_context);

  loop_context.uc_link = &main_context1;

  tealetex_makecontext(&g_scmain, &loop_context, loop, 3, (uintptr_t)&loop_context, (uintptr_t)&main_context2,
                       (uintptr_t)&i_from_iterator);

  iterator_finished = 0;
  tealetex_getcontext(&g_scmain, &main_context1);

  if (!iterator_finished) {
    iterator_finished = 1;

    while (1) {
      tealetex_swapcontext(&g_scmain, &main_context2, &loop_context);
      printf("%d\n", i_from_iterator);
    }
  }

  tealetex_freecontext(&g_scmain, &loop_context);
  return 0;
}
