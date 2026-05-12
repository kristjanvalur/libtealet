#ifndef TEST_RESILIENCE_H
#define TEST_RESILIENCE_H

void test_mem_error(void);
void test_oom_force_marks_source_defunct(void);
void test_oom_force_main_not_defunct(void);
void test_oom_force_peer_then_panic_main(void);
void test_switch_nofail_retries_force(void);
void test_switch_nofail_defunct_target_panics_main(void);
void test_exit_nofail_retries_force(void);
void test_exit_nofail_defunct_target_panics_main(void);
void test_exit_self_invalid(void);

#if TEALET_WITH_TESTING
void test_exit_defunct_target_returns_error(void);
void test_exit_explicit_panic(void);
void test_debug_swap_far_invalid_caller_check_main(void);
void test_debug_swap_far_invalid_caller_check_child(void);
#endif

#endif
