#ifndef TEST_TRANSFER_H
#define TEST_TRANSFER_H

void test_status(void);
void test_exit(void);
void test_switch(void);
void test_switch_self_panic(void);
void test_start_switch_panic_propagates_to_creator(void);
void test_stub_run_panic_propagates_to_creator(void);
void test_arg(void);

#endif