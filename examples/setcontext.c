#include "setcontext.h"

#include <stdarg.h>
#include <stdlib.h>

static void tealetex_dispatch(tealetex_ucontext_t *ucp) {
  switch (ucp->uc_argc) {
  case 0:
    ((void (*)(void))ucp->uc_func)();
    break;
  case 1:
    ((void (*)(uintptr_t))ucp->uc_func)(ucp->uc_argv[0]);
    break;
  case 2:
    ((void (*)(uintptr_t, uintptr_t))ucp->uc_func)(ucp->uc_argv[0], ucp->uc_argv[1]);
    break;
  case 3:
    ((void (*)(uintptr_t, uintptr_t, uintptr_t))ucp->uc_func)(ucp->uc_argv[0], ucp->uc_argv[1], ucp->uc_argv[2]);
    break;
  case 4:
    ((void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))ucp->uc_func)(ucp->uc_argv[0], ucp->uc_argv[1],
                                                                          ucp->uc_argv[2], ucp->uc_argv[3]);
    break;
  default:
    break;
  }
}

static tealet_t *tealetex_context_entry(tealet_t *current, void *arg) {
  tealetex_ucontext_t *ucp = (tealetex_ucontext_t *)arg;

  if (ucp == NULL || ucp->uc_func == NULL)
    return current->main;

  ucp->uc_state |= TEALETEX_UCSTATE_ACTIVE;
  ucp->uc_state &= ~TEALETEX_UCSTATE_EXITED;

  /* Match setcontext-style behavior: when the context function returns,
   * control implicitly transfers to uc_link (or back to main if uc_link is NULL).
   */
  tealetex_dispatch(ucp);

  ucp->uc_state &= ~TEALETEX_UCSTATE_ACTIVE;
  ucp->uc_state |= TEALETEX_UCSTATE_EXITED;

  if (ucp->uc_link != NULL && ucp->uc_link->uc_tealet != NULL)
    return ucp->uc_link->uc_tealet;
  return current->main;
}

static int tealetex_transfer_to(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp, void **parg) {
  void *start_arg;
  int result;

  if (scmain == NULL || scmain->main == NULL || ucp == NULL)
    return TEALET_ERR_INVAL;
  if (ucp->uc_main != scmain->main)
    return TEALET_ERR_INVAL;
  if ((ucp->uc_state & TEALETEX_UCSTATE_BOUND) == 0)
    return TEALET_ERR_INVAL;
  if (ucp->uc_tealet == NULL)
    return TEALET_ERR_INVAL;
  if (ucp->uc_state & TEALETEX_UCSTATE_EXITED)
    return TEALET_ERR_INVAL;

  if (ucp->uc_tealet == tealet_current(scmain->main))
    return 0;

  if ((ucp->uc_state & TEALETEX_UCSTATE_ACTIVE) || ucp->uc_func == NULL)
    return tealet_switch(ucp->uc_tealet, parg, TEALET_XFER_DEFAULT);

  /* First start always passes the context descriptor to the entry wrapper. */
  start_arg = (void *)ucp;

  result = tealet_run(ucp->uc_tealet, tealetex_context_entry, NULL, NULL, TEALET_START_DEFAULT);
  if (result == 0)
    result = tealet_switch(ucp->uc_tealet, &start_arg, TEALET_XFER_DEFAULT);
  if (result == 0 && parg != NULL)
    *parg = start_arg;

  return result;
}

int tealetex_getcontext_init(tealetex_setcontext_main_t *scmain, void *far_boundary) {
  tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;

  if (scmain == NULL || far_boundary == NULL)
    return TEALET_ERR_INVAL;

  scmain->main = NULL;

  scmain->main = tealet_initialize(&alloc, 0);
  if (scmain->main == NULL)
    return TEALET_ERR_MEM;

  if (tealet_set_far(scmain->main, far_boundary) != 0) {
    tealet_finalize(scmain->main);
    scmain->main = NULL;
    return TEALET_ERR_INVAL;
  }

  return 0;
}

void tealetex_getcontext_fini(tealetex_setcontext_main_t *scmain) {
  if (scmain == NULL || scmain->main == NULL)
    return;

  tealet_finalize(scmain->main);
  scmain->main = NULL;
}

int tealetex_getcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp) {
  tealet_t *child;
  int fork_result;
  int i;

  if (scmain == NULL || scmain->main == NULL || ucp == NULL)
    return TEALET_ERR_INVAL;

  child = tealet_new(scmain->main);
  if (child == NULL)
    return TEALET_ERR_MEM;

  ucp->uc_tealet = child;
  ucp->uc_main = scmain->main;
  ucp->uc_link = NULL;
  ucp->uc_func = NULL;
  ucp->uc_argc = 0;
  for (i = 0; i < TEALETEX_MAKECONTEXT_MAX_ARGS; ++i)
    ucp->uc_argv[i] = (uintptr_t)0;

  fork_result = tealet_fork(child, NULL, TEALET_START_DEFAULT);
  if (fork_result != 0) {
    tealet_delete(child);
    ucp->uc_tealet = NULL;
    ucp->uc_main = NULL;
    ucp->uc_state = TEALETEX_UCSTATE_EMPTY;
    return fork_result;
  }

  if (tealet_current(scmain->main) == child)
    ucp->uc_state = TEALETEX_UCSTATE_BOUND | TEALETEX_UCSTATE_ACTIVE;
  else
    ucp->uc_state = TEALETEX_UCSTATE_BOUND;

  return 0;
}

int tealetex_makecontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp,
                         tealetex_context_func_t func, int argc, ...) {
  tealet_t *old_tealet;
  tealet_t *new_tealet;
  va_list ap;
  int i;

  if (scmain == NULL || scmain->main == NULL || ucp == NULL || func == NULL)
    return TEALET_ERR_INVAL;
  if (argc < 0 || argc > TEALETEX_MAKECONTEXT_MAX_ARGS)
    return TEALET_ERR_INVAL;

  if (ucp->uc_main != NULL && ucp->uc_main != scmain->main)
    return TEALET_ERR_INVAL;

  old_tealet = ucp->uc_tealet;
  if (old_tealet != NULL && old_tealet != scmain->main && old_tealet != tealet_current(scmain->main))
    tealet_delete(old_tealet);

  new_tealet = tealet_new(scmain->main);
  if (new_tealet == NULL)
    return TEALET_ERR_MEM;

  ucp->uc_tealet = new_tealet;
  ucp->uc_main = scmain->main;
  ucp->uc_func = func;
  ucp->uc_argc = argc;
  for (i = 0; i < TEALETEX_MAKECONTEXT_MAX_ARGS; ++i)
    ucp->uc_argv[i] = (uintptr_t)0;

  va_start(ap, argc);
  for (i = 0; i < argc; ++i)
    ucp->uc_argv[i] = (uintptr_t)va_arg(ap, uintptr_t);
  va_end(ap);

  ucp->uc_state = TEALETEX_UCSTATE_BOUND;
  return 0;
}

int tealetex_swapcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *oucp, tealetex_ucontext_t *ucp) {
  if (scmain == NULL || scmain->main == NULL || ucp == NULL)
    return TEALET_ERR_INVAL;

  if (oucp != NULL) {
    oucp->uc_tealet = tealet_current(scmain->main);
    oucp->uc_main = scmain->main;
    oucp->uc_state = TEALETEX_UCSTATE_BOUND | TEALETEX_UCSTATE_ACTIVE;
  }

  return tealetex_transfer_to(scmain, ucp, NULL);
}

int tealetex_setcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp, void **parg) {
  return tealetex_transfer_to(scmain, ucp, parg);
}

void tealetex_freecontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp) {
  int i;

  if (scmain == NULL || scmain->main == NULL || ucp == NULL)
    return;

  if (ucp->uc_tealet != NULL && ucp->uc_tealet != scmain->main) {
    if (ucp->uc_tealet != tealet_current(scmain->main))
      tealet_delete(ucp->uc_tealet);
  }

  ucp->uc_tealet = NULL;
  ucp->uc_main = NULL;
  ucp->uc_link = NULL;
  ucp->uc_func = NULL;
  ucp->uc_argc = 0;
  for (i = 0; i < TEALETEX_MAKECONTEXT_MAX_ARGS; ++i)
    ucp->uc_argv[i] = (uintptr_t)0;
  ucp->uc_state = TEALETEX_UCSTATE_EMPTY;
}
