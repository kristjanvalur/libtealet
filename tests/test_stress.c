#include <assert.h>
#include <stdlib.h>
#if !defined _MSC_VER || _MSC_VER >= 1600 /* VS2010 and above */
#include <stdint.h>
#endif

#include "tealet.h"
#include "test_harness.h"

#define ARRAYSIZE 127
#define MAX_STATUS 50000

static tealet_t *tealetarray[ARRAYSIZE] = {NULL};
static int got_index;

static tealet_t *random_new_tealet(tealet_t *, void *arg);

static void random_run(int index) {
  int i, prevstatus;
  void *arg;
  tealet_t *cur = tealet_current(g_main);
  assert(tealetarray[index] == cur);
  do {
    i = rand() % (ARRAYSIZE + 1);
    status += 1;

    /* Check stats periodically */
    if (status % 100 == 0)
      check_stats(0);

    if (i == ARRAYSIZE)
      break;
    prevstatus = status;
    got_index = i;
    if (tealetarray[i] == NULL) {
      if (status >= MAX_STATUS)
        break;
      arg = (void *)(intptr_t)i;
      assert(tealet_new_native_call(g_main, random_new_tealet, &arg, NULL) != NULL);
    } else {
      tealet_switch(tealetarray[i], NULL, TEALET_XFER_DEFAULT);
    }
    assert(status >= prevstatus);
    assert(tealet_current(g_main) == cur);
    assert(tealetarray[index] == cur);
    assert(got_index == index);
  } while (status < MAX_STATUS);
}

static tealet_t *random_new_tealet(tealet_t *cur, void *arg) {
  int i = got_index;
  assert(tealet_current(g_main) == cur);
  assert(i == (intptr_t)(arg));
  assert(i > 0 && i < ARRAYSIZE);
  assert(tealetarray[i] == NULL);
  tealetarray[i] = cur;
  random_run(i);
  tealetarray[i] = NULL;

  i = rand() % ARRAYSIZE;
  if (tealetarray[i] == NULL) {
    assert(tealetarray[0] != NULL);
    i = 0;
  }
  got_index = i;
  tealet_exit(tealetarray[i], NULL, TEALET_EXIT_DELETE);
  abort();
  return NULL;
}

void test_random(void) {
  int i;
  init_test();
  for (i = 0; i < ARRAYSIZE; i++)
    tealetarray[i] = NULL;
  tealetarray[0] = g_main;
  status = 0;
  while (status < MAX_STATUS)
    random_run(0);

  assert(g_main == tealetarray[0]);
  for (i = 1; i < ARRAYSIZE; i++)
    while (tealetarray[i] != NULL)
      random_run(0);

  print_final_stats();
  fini_test();
}

/* Another random switching test.  Tealets find a random target
 * tealet to switch to and do that.  While the test is running, they
 * generate new tealets to fill the array.  Each tealet runs x times
 * and then exits.  The switching happens at a random depth.
 */
#define N_RUNS 10
#define MAX_DESCEND 20

static void random2_run(int index);
static tealet_t *random2_tealet(tealet_t *cur, void *arg) {
  int index = (int)(intptr_t)arg;
  assert(tealet_current(g_main) == cur);
  assert(index > 0 && index < ARRAYSIZE);
  assert(tealetarray[index] == NULL);
  tealetarray[index] = cur;
  random2_run(index);
  tealetarray[index] = NULL;
  tealet_exit(tealetarray[0], NULL, TEALET_EXIT_DELETE); /* switch to main */
  abort();
  return NULL;
}

static void random2_new(int index) {
  void *arg = (void *)(intptr_t)index;
  assert(tealet_new_native_call(g_main, random2_tealet, &arg, NULL) != NULL);
}

static int random2_descend(int index, int level) {
  int target;
  if (level > 0)
    return random2_descend(index, level - 1);

  /* find target */
  target = rand() % ARRAYSIZE;
  if (status < MAX_STATUS) {
    status += 1;
    while (target == index)
      target = rand() % ARRAYSIZE;
    if (tealetarray[target] == NULL)
      random2_new(target);
    else
      tealet_switch(tealetarray[target], NULL, TEALET_XFER_DEFAULT);
    return 1;
  } else {
    /* find a tealet */
    int j;
    for (j = 0; j < ARRAYSIZE; j++) {
      int k = (j + target) % ARRAYSIZE;
      if (k != index && tealetarray[k]) {
        status += 1;
        tealet_switch(tealetarray[k], NULL, TEALET_XFER_DEFAULT);
        return 1;
      }
    }
    return 0;
  }
}

static void random2_run(int index) {
  int i;
  assert(tealetarray[index] == NULL || tealetarray[index] == tealet_current(g_main));
  tealetarray[index] = tealet_current(g_main);
  for (i = 0; i < N_RUNS; i++) {
    /* Check stats periodically */
    if (status % 100 == 0)
      check_stats(0);

    if (random2_descend(index, rand() % (MAX_DESCEND + 1)) == 0)
      break;
  }
  tealetarray[index] = NULL;
}

void test_random2(void) {
  int i;
  init_test();
  for (i = 0; i < ARRAYSIZE; i++)
    tealetarray[i] = NULL;
  tealetarray[0] = g_main;

  while (status < MAX_STATUS)
    random2_run(0);

  /* drain the system */
  tealetarray[0] = tealet_current(g_main);
  for (;;) {
    for (i = 1; i < ARRAYSIZE; i++)
      if (tealetarray[i]) {
        status++;
        tealet_switch(tealetarray[i], NULL, TEALET_XFER_DEFAULT);
        break;
      }
    if (i == ARRAYSIZE)
      break;
  }
  tealetarray[0] = NULL;
  print_final_stats();
  fini_test();
}