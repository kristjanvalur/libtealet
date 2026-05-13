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

/* Completion flag shared between parent path and getcontext continuation path. */
static volatile int iterator_finished;

/* Iterator function. It yields values and switches back to main_context2.
 * When it returns, control flows to loop_context.uc_link (main_context1).
 */
static void loop(tealetex_ucontext_t *loop_context, tealetex_ucontext_t *other_context,
                 int *i_from_iterator_ptr) {
  int i;

  fprintf(stderr,
          "[debug] loop enter: loop_context=%p uc_link=%p expected_main_context1=%p other_context=%p\n",
          (void *)loop_context,
          loop_context != NULL ? (void *)loop_context->uc_link : (void *)0,
          (void *)&main_context1,
          (void *)other_context);

  if (loop_context == NULL || other_context == NULL || i_from_iterator_ptr == NULL)
    return;

  if (loop_context->uc_link != &main_context1) {
    fprintf(stderr, "[debug] WARNING: loop_context->uc_link is not main_context1\n");
  }

  for (i = 0; i < 10; ++i) {
    *i_from_iterator_ptr = i;

    tealetex_swapcontext(&g_scmain, loop_context, other_context, NULL);
  }

  /* The wrapper in examples/setcontext.c applies implicit uc_link transfer
   * when this function returns, like canonical setcontext examples.
   */
}

int main(int argc, char **argv) {
  int main1_resume_count;

  (void)argc;

  main1_resume_count = 0;

  tealetex_getcontext_init(&g_scmain, (void *)&argv);
  tealetex_getcontext(&g_scmain, &loop_context);

  loop_context.uc_link = &main_context1;

  tealetex_makecontext(&g_scmain, &loop_context, loop, 3, (uintptr_t)&loop_context, (uintptr_t)&main_context2,
                       (uintptr_t)&i_from_iterator);

  iterator_finished = 0;
  tealetex_getcontext(&g_scmain, &main_context1);
  ++main1_resume_count;
  fprintf(stderr,
          "[debug] resumed at main_context1: count=%d iterator_finished=%d\n",
          main1_resume_count,
          (int)iterator_finished);

  if (!iterator_finished) {
    fprintf(stderr,
            "[debug] entering producer/consumer loop block at main_context1: count=%d\n",
            main1_resume_count);
    iterator_finished = 1;
    fprintf(stderr,
            "[debug] iterator_finished set to %d before while loop\n",
            (int)iterator_finished);

    while (1) {
      tealetex_swapcontext(&g_scmain, &main_context2, &loop_context, NULL);
      printf("%d\n", i_from_iterator);
    }
  } else {
    fprintf(stderr,
            "[debug] continuation resumed at main_context1 with iterator_finished=%d; skipping loop block\n",
            (int)iterator_finished);
  }

  tealetex_freecontext(&g_scmain, &loop_context);
  return 0;
}
