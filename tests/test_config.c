/* Test tealet_configure_get/set API behavior */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#if !defined _MSC_VER || _MSC_VER >= 1600 /* VS2010 and above */
#include <stdint.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif

#include "tealet.h"

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) do { \
    printf("Running test: %s\n", name); \
    test_count++; \
} while(0)

#define PASS() do { \
    printf("  PASSED\n"); \
    test_passed++; \
} while(0)

static tealet_t *new_main(void)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main_tealet;

    main_tealet = tealet_initialize(&alloc, 0);
    assert(main_tealet != NULL);
    return main_tealet;
}

typedef struct parent_scratch_t {
    unsigned char bytes[64];
} parent_scratch_t;

typedef struct write_command_t {
    tealet_t *return_to;
    unsigned char *outside_target;
    int write_inside;
    int *first_switch_result;
    int *recovery_switch_result;
} write_command_t;

static tealet_t *run_write_to_target(tealet_t *current, void *arg)
{
    write_command_t *command = (write_command_t *)arg;
    unsigned char *target;
    unsigned char original;
    void *far;
    int switch_result;

    if (command->write_inside) {
        far = tealet_get_far(current);
        assert(far != NULL);
#if STACK_DIRECTION == 0
        target = (unsigned char *)far;
#else
        target = ((unsigned char *)far) - 1;
#endif
    } else {
        target = command->outside_target;
    }

    original = *target;
    *target ^= 0x5Au;

    switch_result = tealet_switch(command->return_to, NULL);
    if (command->first_switch_result)
        *command->first_switch_result = switch_result;

    if (switch_result == TEALET_ERR_INTEGRITY) {
        *target = original;
        switch_result = tealet_switch(command->return_to, NULL);
    }

    if (command->recovery_switch_result)
        *command->recovery_switch_result = switch_result;

    return current->main;
}

#if TEALET_WITH_STACK_SNAPSHOT
static int run_integrity_switch_case(int fail_policy, int write_inside,
    int *first_result, int *recovery_result)
{
    tealet_t *main_tealet;
    tealet_t *writer;
    tealet_config_t cfg = TEALET_CONFIG_INIT;
    write_command_t command;
    parent_scratch_t parent_scratch;
    void *arg;
    int result;
    int first_switch_result;
    int second_switch_result;

    main_tealet = new_main();
    memset(&parent_scratch, 0x22, sizeof(parent_scratch));

    cfg.flags = TEALET_CONFIGF_STACK_INTEGRITY | TEALET_CONFIGF_STACK_SNAPSHOT;
    cfg.stack_integrity_bytes = 1;
    cfg.stack_guard_mode = TEALET_STACK_GUARD_MODE_NONE;
    cfg.stack_integrity_fail_policy = fail_policy;
    result = tealet_configure_set(main_tealet, &cfg);
    assert(result == 0);
    assert((cfg.flags & TEALET_CONFIGF_STACK_INTEGRITY) != 0);
    assert((cfg.flags & TEALET_CONFIGF_STACK_SNAPSHOT) != 0);

    writer = tealet_create(main_tealet, run_write_to_target, NULL);
    assert(writer != NULL);

    first_switch_result = 0;
    second_switch_result = 0;
    command.return_to = main_tealet;
    command.outside_target = parent_scratch.bytes;
    command.write_inside = write_inside;
    command.first_switch_result = &first_switch_result;
    command.recovery_switch_result = &second_switch_result;

    arg = &command;
    result = tealet_switch(writer, &arg);
    assert(result == 0);
    tealet_finalize(main_tealet);

    if (first_result)
        *first_result = first_switch_result;
    if (recovery_result)
        *recovery_result = second_switch_result;
    return 0;
}
#endif

static void test_get_defaults(void)
{
    tealet_t *main_tealet;
    tealet_config_t cfg = TEALET_CONFIG_INIT;
    int result;

    TEST("test_get_defaults");

    main_tealet = new_main();
    result = tealet_configure_get(main_tealet, &cfg);
    assert(result == 0);

    assert(cfg.version == TEALET_CONFIG_CURRENT_VERSION);
    assert(cfg.flags == 0u);
    assert(cfg.stack_integrity_bytes == 0);
    assert(cfg.stack_guard_mode == TEALET_STACK_GUARD_MODE_NONE);
    assert(cfg.stack_integrity_fail_policy == TEALET_STACK_INTEGRITY_FAIL_ASSERT);

    tealet_finalize(main_tealet);
    PASS();
}

static void test_set_canonicalizes_unsupported(void)
{
    tealet_t *main_tealet;
    tealet_config_t set_cfg = TEALET_CONFIG_INIT;
    tealet_config_t get_cfg = TEALET_CONFIG_INIT;
    unsigned int expected_flags;
    int result;

    TEST("test_set_canonicalizes_unsupported");

    main_tealet = new_main();

    set_cfg.flags = TEALET_CONFIGF_STACK_INTEGRITY |
                    TEALET_CONFIGF_STACK_GUARD |
                    TEALET_CONFIGF_STACK_SNAPSHOT;
    set_cfg.stack_integrity_bytes = 4096;
    set_cfg.stack_guard_mode = TEALET_STACK_GUARD_MODE_NOACCESS;
    set_cfg.stack_integrity_fail_policy = TEALET_STACK_INTEGRITY_FAIL_ABORT;

    result = tealet_configure_set(main_tealet, &set_cfg);
    assert(result == 0);

    expected_flags = 0u;
#if TEALET_WITH_STACK_GUARD && !defined(_WIN32)
    expected_flags |= TEALET_CONFIGF_STACK_GUARD;
#endif
#if TEALET_WITH_STACK_SNAPSHOT
    expected_flags |= TEALET_CONFIGF_STACK_SNAPSHOT;
#endif
    if (expected_flags != 0u)
        expected_flags |= TEALET_CONFIGF_STACK_INTEGRITY;

    assert(set_cfg.flags == expected_flags);
    if (expected_flags == 0u)
        assert(set_cfg.stack_integrity_bytes == 0);
    else
        assert(set_cfg.stack_integrity_bytes == 4096);

#if TEALET_WITH_STACK_GUARD && !defined(_WIN32)
    assert(set_cfg.stack_guard_mode == TEALET_STACK_GUARD_MODE_NOACCESS);
#else
    assert(set_cfg.stack_guard_mode == TEALET_STACK_GUARD_MODE_NONE);
#endif

    result = tealet_configure_get(main_tealet, &get_cfg);
    assert(result == 0);
    assert(get_cfg.flags == expected_flags);
    assert(get_cfg.stack_integrity_bytes == set_cfg.stack_integrity_bytes);
    assert(get_cfg.stack_guard_mode == set_cfg.stack_guard_mode);

    tealet_finalize(main_tealet);
    PASS();
}

static void test_set_snapshot_supported_build(void)
{
#if TEALET_WITH_STACK_SNAPSHOT
    tealet_t *main_tealet;
    tealet_config_t cfg = TEALET_CONFIG_INIT;
    int result;

    TEST("test_set_snapshot_supported_build");

    main_tealet = new_main();

    cfg.flags = TEALET_CONFIGF_STACK_INTEGRITY | TEALET_CONFIGF_STACK_SNAPSHOT;
    cfg.stack_integrity_bytes = 2048;
    cfg.stack_guard_mode = TEALET_STACK_GUARD_MODE_NONE;
    result = tealet_configure_set(main_tealet, &cfg);
    assert(result == 0);
    assert((cfg.flags & TEALET_CONFIGF_STACK_SNAPSHOT) != 0);
    assert((cfg.flags & TEALET_CONFIGF_STACK_INTEGRITY) != 0);
    assert(cfg.stack_integrity_bytes == 2048);

    tealet_finalize(main_tealet);
    PASS();
#endif
}

static void test_snapshot_soft_error_inside_watch_range(void)
{
#if TEALET_WITH_STACK_SNAPSHOT
    int first_result;
    int recovery_result;

    TEST("test_snapshot_soft_error_inside_watch_range");

    first_result = 0;
    recovery_result = TEALET_ERR_INTEGRITY;
    assert(run_integrity_switch_case(TEALET_STACK_INTEGRITY_FAIL_ERROR, 1,
        &first_result, &recovery_result) == 0);
    assert(first_result == TEALET_ERR_INTEGRITY);
    assert(recovery_result == 0);

    PASS();
#endif
}

static void test_snapshot_write_outside_watch_range_ok(void)
{
#if TEALET_WITH_STACK_SNAPSHOT
    int first_result;
    int recovery_result;

    TEST("test_snapshot_write_outside_watch_range_ok");

    first_result = TEALET_ERR_INTEGRITY;
    recovery_result = TEALET_ERR_INTEGRITY;
    assert(run_integrity_switch_case(TEALET_STACK_INTEGRITY_FAIL_ERROR, 0,
        &first_result, &recovery_result) == 0);
    assert(first_result == 0);
    assert(recovery_result == 0);

    PASS();
#endif
}

static void test_snapshot_abort_policy_subprocess(void)
{
#if TEALET_WITH_STACK_SNAPSHOT && (defined(__unix__) || defined(__APPLE__))
    pid_t pid;
    int wait_status;
    int child_result;

    TEST("test_snapshot_abort_policy_subprocess");

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        child_result = 0;
        run_integrity_switch_case(TEALET_STACK_INTEGRITY_FAIL_ABORT, 1, &child_result, NULL);
        _exit(111);
    }

    assert(waitpid(pid, &wait_status, 0) == pid);
    assert(WIFSIGNALED(wait_status));
    assert(WTERMSIG(wait_status) == SIGABRT);

    PASS();
#elif TEALET_WITH_STACK_SNAPSHOT
    TEST("test_snapshot_abort_policy_subprocess");
    printf("  SKIPPED (requires fork())\n");
    test_passed++;
#endif
}

static void test_set_invalid_version(void)
{
    tealet_t *main_tealet;
    tealet_config_t cfg = TEALET_CONFIG_INIT;
    int result;

    TEST("test_set_invalid_version");

    main_tealet = new_main();

    cfg.version = TEALET_CONFIG_VERSION_1 + 1;
    result = tealet_configure_set(main_tealet, &cfg);
    assert(result == TEALET_ERR_INVAL);

    tealet_finalize(main_tealet);
    PASS();
}

static void test_header_size_validation(void)
{
    tealet_t *main_tealet;
    tealet_config_t cfg = TEALET_CONFIG_INIT;
    int result;

    TEST("test_header_size_validation");

    main_tealet = new_main();

    cfg.size = offsetof(tealet_config_t, version);
    result = tealet_configure_get(main_tealet, &cfg);
    assert(result == TEALET_ERR_INVAL);

    cfg.size = offsetof(tealet_config_t, version);
    cfg.version = TEALET_CONFIG_VERSION_1;
    result = tealet_configure_set(main_tealet, &cfg);
    assert(result == TEALET_ERR_INVAL);

    tealet_finalize(main_tealet);
    PASS();
}

int main(void)
{
    printf("=== Testing tealet_configure API ===\n\n");

    test_get_defaults();
    printf("\n");

    test_set_canonicalizes_unsupported();
    printf("\n");

    test_set_snapshot_supported_build();
    if (test_count > test_passed)
        printf("\n");

    test_snapshot_soft_error_inside_watch_range();
    if (test_count > test_passed)
        printf("\n");

    test_snapshot_write_outside_watch_range_ok();
    if (test_count > test_passed)
        printf("\n");

    test_snapshot_abort_policy_subprocess();
    if (test_count > test_passed)
        printf("\n");

    test_set_invalid_version();
    printf("\n");

    test_header_size_validation();
    printf("\n");

    printf("=== Results: %d/%d tests passed ===\n", test_passed, test_count);

    if (test_passed == test_count) {
        return 0;
    }
    return 1;
}
