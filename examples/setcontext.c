#include "setcontext.h"

#include <stdlib.h>

static tealet_t *tealetex_context_entry(tealet_t *current, void *arg) {
  tealetex_ucontext_t *ucp = (tealetex_ucontext_t *)arg;
  tealet_t *next;

  if (ucp == NULL || ucp->uc_func == NULL)
    return current->main;

  ucp->uc_state |= TEALETEX_UCSTATE_ACTIVE;
  ucp->uc_state &= ~TEALETEX_UCSTATE_EXITED;

  next = ucp->uc_func(current, ucp->uc_arg);

  ucp->uc_state &= ~TEALETEX_UCSTATE_ACTIVE;
  ucp->uc_state |= TEALETEX_UCSTATE_EXITED;

  if (next != NULL)
    return next;
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

  if (ucp->uc_state & TEALETEX_UCSTATE_ACTIVE)
    return tealet_switch(ucp->uc_tealet, parg, TEALET_XFER_DEFAULT);

  if (ucp->uc_func == NULL)
    return TEALET_ERR_INVAL;

  /* First start always passes the context descriptor to the entry wrapper. */
  start_arg = (void *)ucp;

  if (ucp->uc_start_flags & TEALET_START_SWITCH) {
    result = tealet_run(ucp->uc_tealet, tealetex_context_entry, &start_arg, ucp->uc_stack_far, TEALET_START_SWITCH);
    if (result == 0 && parg != NULL)
      *parg = start_arg;
  } else {
    result = tealet_run(ucp->uc_tealet, tealetex_context_entry, NULL, ucp->uc_stack_far, TEALET_START_DEFAULT);
    if (result == 0)
      result = tealet_switch(ucp->uc_tealet, &start_arg, TEALET_XFER_DEFAULT);
    if (result == 0 && parg != NULL)
      *parg = start_arg;
  }

  return result;
}

int tealetex_getcontext_init(tealetex_setcontext_main_t *scmain) {
  tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;

  if (scmain == NULL)
    return TEALET_ERR_INVAL;

  scmain->main = NULL;

  scmain->main = tealet_initialize(&alloc, 0);
  if (scmain->main == NULL)
    return TEALET_ERR_MEM;
  return 0;
}

void tealetex_getcontext_fini(tealetex_setcontext_main_t *scmain) {
  if (scmain == NULL || scmain->main == NULL)
    return;

  tealet_finalize(scmain->main);
  scmain->main = NULL;
}

int tealetex_getcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp) {
  tealet_t *current;

  if (scmain == NULL || scmain->main == NULL || ucp == NULL)
    return TEALET_ERR_INVAL;

  current = tealet_current(scmain->main);
  ucp->uc_tealet = current;
  ucp->uc_main = scmain->main;
  ucp->uc_link = NULL;
  ucp->uc_func = NULL;
  ucp->uc_arg = NULL;
  ucp->uc_stack_far = NULL;
  ucp->uc_start_flags = TEALET_START_DEFAULT;
  ucp->uc_state = TEALETEX_UCSTATE_BOUND | TEALETEX_UCSTATE_ACTIVE;
  return 0;
}

int tealetex_makecontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp, tealet_run_t func, void *arg,
                         void *stack_far, int start_flags) {
  tealet_t *new_tealet;

  if (scmain == NULL || scmain->main == NULL || ucp == NULL || func == NULL)
    return TEALET_ERR_INVAL;
  if ((start_flags & ~TEALET_START_SWITCH) != 0)
    return TEALET_ERR_INVAL;

  new_tealet = tealet_new(scmain->main);
  if (new_tealet == NULL)
    return TEALET_ERR_MEM;

  ucp->uc_tealet = new_tealet;
  ucp->uc_main = scmain->main;
  ucp->uc_func = func;
  ucp->uc_arg = arg;
  ucp->uc_stack_far = stack_far;
  ucp->uc_start_flags = start_flags;
  ucp->uc_state = TEALETEX_UCSTATE_BOUND;
  return 0;
}

int tealetex_swapcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *oucp, tealetex_ucontext_t *ucp,
                         void **parg) {
  if (scmain == NULL || scmain->main == NULL || ucp == NULL)
    return TEALET_ERR_INVAL;

  if (oucp != NULL) {
    oucp->uc_tealet = tealet_current(scmain->main);
    oucp->uc_main = scmain->main;
    oucp->uc_link = NULL;
    oucp->uc_func = NULL;
    oucp->uc_arg = NULL;
    oucp->uc_stack_far = NULL;
    oucp->uc_start_flags = TEALET_START_DEFAULT;
    oucp->uc_state = TEALETEX_UCSTATE_BOUND | TEALETEX_UCSTATE_ACTIVE;
  }

  return tealetex_transfer_to(scmain, ucp, parg);
}

int tealetex_setcontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp, void **parg) {
  return tealetex_transfer_to(scmain, ucp, parg);
}

void tealetex_freecontext(tealetex_setcontext_main_t *scmain, tealetex_ucontext_t *ucp) {
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
  ucp->uc_arg = NULL;
  ucp->uc_stack_far = NULL;
  ucp->uc_start_flags = TEALET_START_DEFAULT;
  ucp->uc_state = TEALETEX_UCSTATE_EMPTY;
}
