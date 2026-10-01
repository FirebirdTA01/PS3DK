/*! \file ps3tc_mm_lasterror.h
 \brief PS3 Custom Toolchain (SDK-only) RSX heap last-error diagnostic
        query.  Not part of the PSL1GHT public API.

The PSL1GHT contract in \c rsx/mm.h gives the caller no way to tell a
not-initialised heap, an exhausted one, or a rejected free pointer,
because the signatures are bool, NULL and void.  This small query
pair provides a stable code plus a short human-readable description
for each failure mode, without changing any of those signatures.

Link-time contract: the two query functions and the \c rsx* allocator
calls they report on are defined in the *same object* (the mm.o of
whichever archive is linked).  A static archive only pulls in an
object when it resolves a symbol, so a link line with both -lrsx and
-lgcm_cmd pulls mm.o from the first archive that satisfies rsxMalloc
and never pulls the second; \c ps3tc_rsx_mm_last_error therefore
resolves to the object whose static it observes, and the pair can
never be split across the two libraries.  There is no duplicate-symbol
error in either layout, so the guarantee is co-location in one object,
not a link failure.

Thread-safety: the code is a process-global word that the allocators
write with __ATOMIC_RELAXED stores and the query reads with a matching
load, so concurrent threads cannot corrupt it.  It is therefore the
most recent outcome from ANY thread (last writer wins), well-defined
but not per-thread; there is no thread-local result.  The store is
performed by each allocator after the shared heap has released its
internal lock, so a reader observes some complete store, never torn,
on the PPU it compiles to a plain stw/lwz.

Known limitation: rsxHeapInit does not check heapInit's return value,
so a heapInit failure (which in practice never happens for this
allocator) is not recorded as an error; a successful rsxHeapInit
still leaves the code at PS3TC_RSX_MM_LASTERROR_OK.
*/

#ifndef __PS3TC_RSX_MM_LASTERROR_H__
#define __PS3TC_RSX_MM_LASTERROR_H__

#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Most recent RSX heap call (or heap init) succeeded. */
#define PS3TC_RSX_MM_LASTERROR_OK              0
/*! \ref rsxHeapInit: gcmGetConfiguration returned non-zero. */
#define PS3TC_RSX_MM_LASTERROR_GCM_CONFIG     -1
/*! \ref rsxMalloc / \ref rsxMemalign / \ref rsxFree: the heap was
    not initialised (rsxHeapInit was not called, or failed). */
#define PS3TC_RSX_MM_LASTERROR_UNINIT         -2
/*! \ref rsxMalloc / \ref rsxMemalign: the RSX heap could not satisfy
    the request (heap exhausted, or a size/alignment computation
    overflowed). */
#define PS3TC_RSX_MM_LASTERROR_ALLOCATION     -3
/*! \ref rsxFree: the pointer was not in the RSX heap (an interior or
    out-of-range value the allocator's free path rejects; rsxFree(NULL)
    is a no-op and records OK). */
#define PS3TC_RSX_MM_LASTERROR_FREE           -4

/*! \brief Return the code recorded by the most recent RSX heap call.

The code is sticky: it is set on the most recent failure and cleared
to \c PS3TC_RSX_MM_LASTERROR_OK on the most recent success, so the
returned value reflects the outcome of the most recent heap call made
on this process.  If rsxHeapInit has not run yet, the value is
\c PS3TC_RSX_MM_LASTERROR_OK.

\return One of the \c PS3TC_RSX_MM_LASTERROR_* codes.
*/
s32 ps3tc_rsx_mm_last_error(void);

/*! \brief Return a short description of an RSX heap last-error code.
\param code A \c PS3TC_RSX_MM_LASTERROR_* code.  Unrecognised codes
        map to "unknown last error".
\return A static ASCII string (may be aliased).  Never \c NULL.
*/
const char *ps3tc_rsx_mm_last_error_string(s32 code);

#ifdef __cplusplus
	}
#endif

#endif /* __PS3TC_RSX_MM_LASTERROR_H__ */
