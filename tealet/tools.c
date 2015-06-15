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

