#include "tools.h"

/* the stats allocator, used to collect memory usage statistics */
void *
tealet_statsalloc_malloc(size_t size, void *context)
{
    size_t nsize;
    void *result;
    tealet_statsalloc_t *alloc = (tealet_statsalloc_t *)context;
    nsize = size + 8;/* assume 64 bit alignment */
    result = TEALET_ALLOC_MALLOC(alloc->base, nsize);
    if (result == NULL)
        return result;
    alloc->n_allocs += 1;
    alloc->s_allocs += size;
    * (size_t*)result = size;
    result = (void*) ((char*)result + 8);
    return result;
}

void
tealet_statsalloc_free(void *ptr, void *context)
{
    size_t size;
    tealet_statsalloc_t *alloc = (tealet_statsalloc_t *)context;
    if (ptr == NULL)
        return;
    ptr = (void*) ((char*)ptr - 8);
    size = * (size_t*)ptr;
    alloc->n_allocs -= 1;
    alloc->s_allocs -= size;
    TEALET_ALLOC_FREE(alloc->base, ptr);
}


void
tealet_statsalloc_init(tealet_statsalloc_t *alloc, tealet_alloc_t *base)
{
    alloc->alloc.context = (void*)alloc;
    alloc->alloc.malloc_p = tealet_statsalloc_malloc;
    alloc->alloc.free_p = tealet_statsalloc_free;
    alloc->base = base;
    alloc->n_allocs = alloc->s_allocs = 0;
}

/****************************************************************
 * Implement copyable stubs by using a trampoline
 * A stub is a special paused tealet, that can be restarted to
 * run any function.  It can also be duplicated, providing a
 * convenient mechanism to start a family of tealets from a common
 * position on the stack.
 */
struct stub_arg
{
    tealet_t *current;
    tealet_run_t run;
    void *runarg;
};
static tealet_t *
_tealet_stub_main(tealet_t *current, void *arg)
{
    void *myarg = 0;
    /* original call.
     * the caller is in arg, return right back to him
     */
    tealet_switch((tealet_t*)arg, &myarg);

    /* now we are back, through a call to tealet_stub_run.  We may be 
     * duplicate of the original stub.
     * myarg should contain the arg to the run function.
     * We were possibly duplicated, so can't trust the original function args.
     */
    {
        struct stub_arg sarg = *(struct stub_arg*)myarg;
        tealet_free(sarg.current, myarg);
        return (sarg.run)(sarg.current, sarg.runarg);
    }
}

/* create a stub and return it */
tealet_t *
tealet_stub_new(tealet_t *t) {
    void *arg = (void*)tealet_current(t);
    return tealet_new(t, _tealet_stub_main, &arg);
}

/*
 * Run a stub.
 * 'stub' must be the result of tealet_stub_new(), otherwise
 * behaviour is undefined.
 */
int
tealet_stub_run(tealet_t *stub, tealet_run_t run, void **parg)
{
    int result;
    void *myarg;
    /* we cannot pass arguments to a different tealet on the stack */
    struct stub_arg *psarg = (struct stub_arg*)tealet_malloc(stub, sizeof(struct stub_arg));
    if (!psarg)
        return TEALET_ERR_MEM;
    psarg->current = stub;
    psarg->run = run;
    psarg->runarg = parg ? *parg : NULL;
    myarg = (void*)psarg;
    result = tealet_switch(stub, &myarg);
    if (result) {
        /* failure */
        tealet_free(stub, psarg);
        return result;
    }
    /* pass back the arg value from the switch */
    if (parg)
        *parg = myarg;
    return 0;
}
