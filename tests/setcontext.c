/*
 *  * This test is an adaption of the example for the setcontext() library
 *   * as illustrated at https://en.wikipedia.org/wiki/Setcontext
 *    * It serves to show how to do a similar thin using tealets
 *     */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "tealet.h"
#include "tools.h"


/* This is the iterator function. It is entered on the first call to
 * swapcontext, and loops from 0 to 9. Each value is saved in i_from_iterator,
 * and then swapcontext used to return to the main loop.  The main loop prints
 * the value and calls swapcontext to swap back into the function. When the end
 * of the loop is reached, the function exits, and execution switches to the
 * context pointed to by main_context1. */
tealet_t *loop_func(
    tealet_t *current,
    void *arg)
{
    int i;
        
    for (i=0; i < (int)(intptr_t)arg; ++i) {
        /* Write the loop counter a variable used for passing between tealets. */
        void *value = (void*) (intptr_t)i;
        
        /* Switch to main tealet */
        tealet_switch(tealet_previous(current), &value);
        /* after switch, any void* passed _to_ us is in 'value' */
    }
    /* exit, without deleting, so that the caller can query status */
    tealet_exit(tealet_previous(current), NULL, TEALET_FLAG_NONE);
    return 0; /* not reached */
} 
 
int main(void)
{
    /* initialize main tealet using malloc based allocation */
    tealet_alloc_t talloc = TEALET_ALLOC_INIT_MALLOC;
    tealet_t *tmain = tealet_initialize(&talloc, 0);
    tealet_t *loop;
    void *data; /* data exchange object */

    /* how many rounds? */
    data = (void*)10;
    loop = tealet_new(tmain, loop_func, &data);
       
    /* loop until the tealet has exited */
    while(tealet_status(loop) == TEALET_STATUS_ACTIVE) {
            /* we don't pass anything _in_ to the loop here,
             * only retrieve the result
             */
            printf("%d\n", (int)(intptr_t)data);
            tealet_switch(loop, &data);
            
        }
    tealet_delete(loop);
    tealet_finalize(tmain);
    return 0;
}
