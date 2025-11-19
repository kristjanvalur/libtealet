/* Test tealet_fork functionality */

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

/* Test basic fork without TEALET_FORK_SWITCH */
static void test_basic_fork(void)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *other = NULL;
    int result;
    int far_marker;
    
    TEST("test_basic_fork");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, &far_marker);
    assert(result == 0);
    
    /* Fork - creates child but stays in parent */
    result = tealet_fork(main, &other, TEALET_FORK_DEFAULT);
    
    if (result == 1) {
        /* We are the parent, other = child */
        tealet_t *child_saved = other; /* Save child pointer before switching */
        assert(other != NULL);
        printf("  Parent: fork returned %d, child=%p, main=%p\n", result, (void*)other, (void*)main);
        printf("  Parent: switching to child...\n");
        
        /* Switch to child */
        result = tealet_switch(other, NULL);
        assert(result == 0);
        
        printf("  Parent: returned from child, other now=%p, child_saved=%p\n", 
               (void*)other, (void*)child_saved);
        printf("  Parent: current=%p, main=%p\n", (void*)tealet_current(main), (void*)main);
        
        /* Clean up - use saved pointer since 'other' was overwritten */
        tealet_delete(child_saved);
        tealet_finalize(main);
        
        PASS();
    } else if (result == 0) {
        /* We are the child, other = parent */
        tealet_t *current_tealet = tealet_current(other);
        printf("  Child: fork returned %d, parent=%p, current=%p\n", 
               result, (void*)other, (void*)current_tealet);
        printf("  Child: exiting via tealet_exit...\n");
        
        /* Child must explicitly exit - no run function to return from */
        tealet_exit(other, NULL, 0);
        
        printf("  Child: this should never print\n");
        assert(0);
    } else {
        /* Error */
        printf("  Error: fork returned %d\n", result);
        assert(0);
    }
}

/* Test fork with TEALET_FORK_SWITCH */
static void test_fork_switch(void)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *other = NULL;
    int result;
    int far_marker;
    int switch_count = 0;
    
    TEST("test_fork_switch");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, &far_marker);
    assert(result == 0);
    
    printf("  Before fork: switch_count=%d\n", switch_count);
    
    /* Fork with immediate switch - becomes child immediately */
    result = tealet_fork(main, &other, TEALET_FORK_SWITCH);
    
    switch_count++;
    
    if (result == 0) {
        /* We are the child */
        tealet_t *current_tealet = tealet_current(main);
        printf("  Child: fork returned %d, switch_count=%d, other=%p\n", 
               result, switch_count, (void*)other);
        printf("  Child: current_tealet=%p, main=%p\n", (void*)current_tealet, (void*)main);
        printf("  Child: exiting via tealet_exit...\n");
        
        /* Child must explicitly exit - no run function to return from */
        tealet_exit(other, NULL, 0);
        
        printf("  Child: this should never print\n");
        assert(0);
    } else {
        /* We are the parent, switched back to */
        assert(result == 1);
        printf("  Parent: fork returned %d, switch_count=%d, other=%p\n", 
               result, switch_count, (void*)other);
        printf("  Parent: back from child, cleaning up\n");
    }
    
    /* Clean up - only parent gets here */
    printf("  Parent: deleting child other=%p\n", (void*)other);
    tealet_delete(other);
    tealet_finalize(main);
    
    PASS();
}

/* Test multiple forks */
static void test_multiple_forks(void)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *child1 = NULL;
    tealet_t *child2 = NULL;
    int result;
    int far_marker;
    int visited = 0;
    
    TEST("test_multiple_forks");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, &far_marker);
    assert(result == 0);
    
    /* Create first child */
    result = tealet_fork(main, &child1, TEALET_FORK_DEFAULT);
    if (result == 0) {
        /* We are child1 */
        printf("  Child1: woke up, exiting\n");
        tealet_exit(child1, NULL, 0);
        assert(0);
    }
    assert(child1 != NULL);
    tealet_t *child1_saved = child1;  /* Save before switching */
    
    printf("  Parent: created child1=%p\n", (void*)child1);
    
    /* Create second child */
    result = tealet_fork(main, &child2, TEALET_FORK_DEFAULT);
    if (result == 0) {
        /* We are child2 */
        printf("  Child2: woke up, exiting\n");
        tealet_exit(child2, NULL, 0);
        assert(0);
    }
    assert(child2 != NULL);
    tealet_t *child2_saved = child2;  /* Save before switching */
    
    printf("  Parent: created child2=%p\n", (void*)child2);
    
    /* Switch to child1 */
    visited++;
    printf("  Parent: switching to child1 (visited=%d)\n", visited);
    tealet_switch(child1, NULL);
    printf("  Parent: returned from child1\n");
    
    /* Switch to child2 */
    visited++;
    printf("  Parent: switching to child2 (visited=%d)\n", visited);
    tealet_switch(child2, NULL);
    printf("  Parent: returned from child2\n");
    
    assert(visited == 2);
    
    /* Clean up - use saved pointers */
    tealet_delete(child1_saved);
    tealet_delete(child2_saved);
    tealet_finalize(main);
    
    PASS();
}

/* Test fork then switch back and forth */
static void test_ping_pong(void)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *child = NULL;
    int result;
    int far_marker;
    int counter = 0;
    
    TEST("test_ping_pong");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, &far_marker);
    assert(result == 0);
    
    /* Fork */
    result = tealet_fork(main, &child, TEALET_FORK_DEFAULT);
    
    counter++;
    
    if (result == 1) {
        /* Parent */
        tealet_t *child_saved = child;  /* Save before switching */
        printf("  Parent: counter=%d, switching to child\n", counter);
        
        /* Ping-pong a few times */
        while (counter < 5) {
            tealet_switch(child, NULL);
            counter++;
            printf("  Parent: counter=%d\n", counter);
        }
        
        printf("  Parent: done, cleaning up\n");
        
        /* Clean up - use saved pointer */
        tealet_delete(child_saved);
        tealet_finalize(main);
        
        PASS();
    } else {
        /* Child */
        assert(result == 0);
        printf("  Child: counter=%d, switching to parent\n", counter);
        
        while (counter < 5) {
            tealet_switch(child, NULL);  /* child pointer is parent from child's perspective */
            counter++;
            printf("  Child: counter=%d, switching to parent\n", counter);
        }
        
        /* Final switch back to parent for cleanup */
        tealet_exit(child, NULL, 0);
    }
}

int main(void)
{
    /* Disable stdout buffering to see debug output before crashes */
    setbuf(stdout, NULL);
    
    printf("=== Testing tealet_fork ===\n\n");
    
    test_basic_fork();
    printf("\n");
    
    test_fork_switch();
    printf("\n");
    
    test_multiple_forks();
    printf("\n");
    
    test_ping_pong();
    printf("\n");
    
    printf("=== Results: %d/%d tests passed ===\n", test_passed, test_count);
    
    return (test_passed == test_count) ? 0 : 1;
}
