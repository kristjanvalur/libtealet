#include "tealet.h"
#ifndef _TEALET_TOOLS_H_
#define _TEALET_TOOLS_H_

typedef struct tealet_statsalloc_t
{
    tealet_alloc_t alloc;
    tealet_alloc_t *base;
    size_t n_allocs;
    size_t s_allocs;
} tealet_statsalloc_t;

TEALET_API
void
tealet_statsalloc_init(tealet_statsalloc_t *alloc, tealet_alloc_t *base);


#endif /* _TEALET_TOOLS_H_ */