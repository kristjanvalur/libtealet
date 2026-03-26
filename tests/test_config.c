/* Test tealet_configure_get/set API behavior */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

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

    /* Current build defaults compile features out, so request is canonicalized off. */
    assert(set_cfg.flags == 0u);
    assert(set_cfg.stack_integrity_bytes == 0);
    assert(set_cfg.stack_guard_mode == TEALET_STACK_GUARD_MODE_NONE);

    result = tealet_configure_get(main_tealet, &get_cfg);
    assert(result == 0);
    assert(get_cfg.flags == 0u);
    assert(get_cfg.stack_integrity_bytes == 0);
    assert(get_cfg.stack_guard_mode == TEALET_STACK_GUARD_MODE_NONE);

    tealet_finalize(main_tealet);
    PASS();
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
