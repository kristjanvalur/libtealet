#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include "tealet.h"

extern int status;
extern tealet_t *g_main;

typedef struct lock_snapshot_t {
  int lock_calls;
  int unlock_calls;
} lock_snapshot_t;

void init_test(void);
void fini_test(void);

void lock_snapshot_take(lock_snapshot_t *snap);
void lock_snapshot_assert_delta_one(const lock_snapshot_t *before);
void test_lock_assert_unheld(void);

tealet_t *tealet_new_native_call(tealet_t *m, tealet_run_t run, void **parg, void *stack_far);

#endif