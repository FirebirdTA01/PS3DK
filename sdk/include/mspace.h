#ifndef __PS3DK_MSPACE_H__
#define __PS3DK_MSPACE_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *mspace;

struct malloc_managed_size {
    size_t current_inuse_size;
    size_t current_system_size;
    size_t max_system_size;
};

mspace mspace_create(void *base, size_t capacity);
int mspace_destroy(mspace msp);
void *mspace_malloc(mspace msp, size_t size);
void mspace_free(mspace msp, void *ptr);
void *mspace_calloc(mspace msp, size_t nelem, size_t size);
void *mspace_memalign(mspace msp, size_t boundary, size_t size);
void *mspace_realloc(mspace msp, void *ptr, size_t size);
void *mspace_reallocalign(mspace msp, void *ptr, size_t size, size_t boundary);
int mspace_malloc_stats(mspace msp, struct malloc_managed_size *mmsize);
int mspace_malloc_stats_fast(mspace msp, struct malloc_managed_size *mmsize);
size_t mspace_malloc_usable_size(void *ptr);
int mspace_is_heap_empty(mspace msp);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_MSPACE_H__ */
