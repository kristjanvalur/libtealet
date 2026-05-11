#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include "tealet.h"

extern int status;
extern tealet_t *g_main;
extern int talloc_fail;
extern tealet_alloc_t talloc;

typedef struct lock_snapshot_t {
  int lock_calls;
  int unlock_calls;
} lock_snapshot_t;

void init_test(void);
void fini_test(void);
void init_test_extra(tealet_alloc_t *alloc, size_t extrasize);

void lock_snapshot_take(lock_snapshot_t *snap);
void lock_snapshot_assert_delta_one(const lock_snapshot_t *before);
void test_lock_assert_unheld(void);
void check_stats(int verbose);
void print_final_stats(void);

tealet_t *tealet_new_native_call(tealet_t *m, tealet_run_t run, void **parg, void *stack_far);
int tealet_new_descend(tealet_t *t, tealet_t **out, int level, tealet_run_t run, void **parg, void *stack_far);
int tealet_test_new_dispatch(tealet_t *t, tealet_t **out, tealet_run_t run, void **parg, void *stack_far);

#endif