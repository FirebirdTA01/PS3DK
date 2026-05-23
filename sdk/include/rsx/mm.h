/*! \file mm.h
\brief RSX memory management.
*/

#ifndef __RSX_MM_H__
#define __RSX_MM_H__

#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! \brief Initialize the RSX heap.
\return 0 if no error, nonzero otherwise.
*/
s32 rsxHeapInit();

/*! \brief Dynamic allocation of RSX memory.
\param size Size in bytes of buffer to be allocated.
\return Pointer to the allocated buffer, or \c NULL if an error occured.
*/
void* rsxMalloc(u32 size);

/*! \brief Dynamic allocation of aligned RSX memory.
\param alignment The required alignment value.
\param size Size in bytes of buffer to be allocated.
\return Pointer to the allocated buffer, or \c NULL if an error occured.
*/
void* rsxMemalign(u32 alignment,u32 size);

/*! \brief Deallocation of a previously dynamically allocated RSX buffer.

The buffer must have been allocated with \ref rsxMalloc or \ref rsxMemalign.
This is an allocator-only operation: it does not unbind render targets,
display buffers, textures, tiles, vertex arrays, or other GPU references.
Before calling \ref rsxFree, callers must ensure the RSX will no longer
read or write the allocation. At process exit, explicit frees for RSX-local
resources are usually unnecessary because LV2 reclaims the process-owned
RSX memory.
\param ptr Pointer to the allocated buffer.
*/
void rsxFree(void *ptr);

#ifdef __cplusplus
	}
#endif

#endif
