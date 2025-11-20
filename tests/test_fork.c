/* Test tealet_fork functionality */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
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
static void test_basic_fork(void* far_marker)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *other = NULL;
    int result;
    
    TEST("test_basic_fork");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, far_marker);
    assert(result == 0);

    int testvalue = 0;
    
    /* Fork - creates child but stays in parent */
    result = tealet_fork(main, &other, TEALET_FORK_DEFAULT);
    
    if (result == 1) {
        /* We are the parent, other = child */
        assert(other != NULL);
        assert(testvalue == 0);  /* Parent should have saved value */
        printf("  Parent: switching to child...\n");
        
        /* Switch to child */
        result = tealet_switch(other, NULL);
        assert(result == 0);
        assert(testvalue == 0);  /* Parent value should be preserved after child modifies it */
        
        printf("  Parent: returned from child, stack preserved correctly\n");
        
        /* Clean up */
        tealet_delete(other);
        tealet_finalize(main);
        
        PASS();
    } else if (result == 0) {
        /* We are the child, other = parent */
        assert(testvalue == 0);  /* Child should start with saved value */
        
        /* Modify stack variable to verify parent's stack is isolated */
        testvalue = 42;
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
static void test_fork_switch(void *far_marker)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *other = NULL;
    int result;
    int switch_count = 0;
    
    TEST("test_fork_switch");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, far_marker);
    assert(result == 0);
    
    printf("  Before fork: switch_count=%d\n", switch_count);
    int testvalue = 0;
    
    /* Fork with immediate switch - becomes child immediately */
    result = tealet_fork(main, &other, TEALET_FORK_SWITCH);
    
    switch_count++;
    
    if (result == 0) {
        /* We are the child */
        assert(testvalue == 0);  /* Child should start with saved value */
        assert(switch_count == 1);  /* Child should have incremented value */
        
        /* Modify stack variables to verify parent's stack is isolated */
        testvalue = 42;
        printf("  Child: exiting via tealet_exit...\n");
        
        /* Child must explicitly exit - no run function to return from */
        tealet_exit(other, NULL, 0);        printf("  Child: this should never print\n");
        assert(0);
    } else {
        /* We are the parent, switched back to */
        assert(result == 1);
        assert(testvalue == 0);  /* Parent should have saved value, not child's 42 */
        assert(switch_count == 1);  /* Parent should have saved value, not child's 2 */
        printf("  Parent: stack variables preserved correctly\n");
        printf("  Parent: back from child, cleaning up\n");
    }    /* Clean up - only parent gets here */
    tealet_delete(other);
    tealet_finalize(main);
    
    PASS();
}

/* Test multiple forks */
static void test_multiple_forks(void *far_marker)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *child1 = NULL;
    tealet_t *child2 = NULL;
    int result;
    int visited = 0;
    
    TEST("test_multiple_forks");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, far_marker);
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
    
    /* Clean up */
    tealet_delete(child1);
    tealet_delete(child2);
    tealet_finalize(main);
    
    PASS();
}

/* Test fork then switch back and forth */
static void test_ping_pong(void *far_marker)
{
    tealet_alloc_t alloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *main;
    tealet_t *child = NULL;
    int result;
    int counter = 0;
    
    TEST("test_ping_pong");
    
    /* Initialize main tealet */
    main = tealet_initialize(&alloc, 0);
    assert(main != NULL);
    
    /* Set far boundary for main tealet */
    result = tealet_set_far(main, far_marker);
    assert(result == 0);
    
    /* Array to verify stack isolation - won't be in registers */
    int data[5] = {0, 0, 0, 0, 0};
    
    /* Fork */
    result = tealet_fork(main, &child, TEALET_FORK_DEFAULT);
    
    counter++;

    if (result == 1) {
        /* Parent */
        tealet_t *child_saved = child;  /* Save - local vars on swapped stack */
        printf("  Parent: counter=%d, data=[%d,%d,%d,%d,%d], switching to child\n", 
               counter, data[0], data[1], data[2], data[3], data[4]);
        
        /* Ping-pong a few times */
        while (counter <= 5) {
            tealet_switch(child_saved, NULL);
            counter++;
            /* Parent increments by 1 each iteration (but only first 4) */
            if (counter <= 5) {
                data[counter - 2]++;
            }
            /* Verify parent's data array is isolated from child modifications */
            printf("  Parent: counter=%d, data=[%d,%d,%d,%d,%d]\n", 
                   counter, data[0], data[1], data[2], data[3], data[4]);
        }
        
        /* Verify final parent data: should be [1,1,1,1,0] */
        assert(data[0] == 1 && data[1] == 1 && data[2] == 1 && data[3] == 1 && data[4] == 0);
        printf("  Parent: data verified as private (correct)\n");
        printf("  Parent: done, cleaning up\n");
        
        /* Clean up */
        tealet_delete(child_saved);
        tealet_finalize(main);
        
        PASS();
    } else {
        /* Child */
        assert(result == 0);
        printf("  Child: counter=%d, data=[%d,%d,%d,%d,%d], switching to parent\n", 
               counter, data[0], data[1], data[2], data[3], data[4]);
        
        while (counter < 5) {
            /* Child increments by 10 each iteration (counter goes 1,2,3,4 -> indices 0,1,2,3) */
            data[counter - 1] += 10;
            tealet_switch(child, NULL);  /* child pointer is parent from child's perspective */
            counter++;
            printf("  Child: counter=%d, data=[%d,%d,%d,%d,%d], switching to parent\n", 
                   counter, data[0], data[1], data[2], data[3], data[4]);
        }
        
        /* Verify final child data: should be [10,10,10,10,0] */
        assert(data[0] == 10 && data[1] == 10 && data[2] == 10 && data[3] == 10 && data[4] == 0);
        printf("  Child: data verified as private (correct)\n");
        
        /* Final switch back to parent for cleanup */
        printf("  Child: exiting\n");
        tealet_exit(child, NULL, 0);
    }
}

int main(void)
{
    /* Disable stdout buffering to see debug output before crashes */
    setbuf(stdout, NULL);
    int far_marker; /* marker for far boundary */
    
    printf("=== Testing tealet_fork ===\n\n");
    
    test_basic_fork(&far_marker);
    printf("\n");
    
    test_fork_switch(&far_marker);
    printf("\n");
    
    test_multiple_forks(&far_marker );
    printf("\n");
    
    test_ping_pong(&far_marker );
    printf("\n");
    
    printf("=== Results: %d/%d tests passed ===\n", test_passed, test_count);
    
    return (test_passed == test_count) ? 0 : 1;
}
