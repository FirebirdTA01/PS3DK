#include <stdlib.h>

#include <gcm_sys.h>
#include <rsx/mm.h>
#include <rsx/ps3tc_mm_lasterror.h>
#include <sys/heap.h>

#ifdef PS3TC_RSX_MM_DEBUG
#  include <stdio.h>
#endif

#define SAFE_AREA			4096

static gcmConfiguration __rsx_config;

static heap_cntrl __rsx_heap;
static u32 __rsxheap_initialized = 0;

/* The last-error code recorded by the most recent RSX heap call.
 * Sticky: cleared to PS3TC_RSX_MM_LASTERROR_OK on the most recent
 * success, set to the matching non-zero code on the most recent
 * failure.  See <rsx/ps3tc_mm_lasterror.h> for the contract.  This
 * static and the ps3tc_rsx_mm_last_error* getters below live in the
 * same object as the rsx*allocator they report on, so the query a
 * caller observes is the one for the allocator that was linked; the
 * pair cannot be split across librsx.a and libgcm_cmd.a.
 * The write goes through __atomic_store_n(..., __ATOMIC_RELAXED) and
 * the query ps3tc_rsx_mm_last_error reads with a matching
 * __atomic_load_n, so concurrent allocators and readers cannot corrupt
 * the word and the reader sees some complete store (last writer wins).
 * On the PPU target these lower to a plain stw / lwz with no fence. */
static s32 __rsx_mm_last_error = PS3TC_RSX_MM_LASTERROR_OK;

static void __rsx_mm_set_last_error(s32 code)
{
#ifdef PS3TC_RSX_MM_DEBUG
	/* One line to stderr.  Gate the *call* as well, so users building
	 * without PS3TC_RSX_MM_DEBUG do not pull in libstdio at all; users
	 * who build with it see a one-shot diagnostic at the failure site,
	 * which is enough to locate a "silent" rsxMalloc/rsxMemalign NULL
	 * or rsxFree rejection without needing a logger. */
	if (code != PS3TC_RSX_MM_LASTERROR_OK)
		fprintf(stderr, "ps3tc rsx mm: %s\n",
				ps3tc_rsx_mm_last_error_string(code));
#endif
	__atomic_store_n(&__rsx_mm_last_error, code, __ATOMIC_RELAXED);
}

s32 rsxHeapInit()
{
	if(!__rsxheap_initialized) {
		void *heapBuffer;
		u32 heapBufferSize;

		/* gcmGetConfiguration returns 0 on success and non-zero on
		 * failure; but its non-zero range is unspecified and could
		 * overlap the documented "1 on success" of rsxHeapInit, so
		 * on failure return 0 (rsxHeapInit's documented error value)
		 * rather than forwarding rc. */
		if (gcmGetConfiguration(&__rsx_config) != 0) {
			__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_GCM_CONFIG);
			return 0;
		}

		heapBuffer = __rsx_config.localAddress;
		heapBufferSize = __rsx_config.localSize - SAFE_AREA;
		heapInit(&__rsx_heap,heapBuffer,heapBufferSize);
		__rsxheap_initialized = 1;
	}

	__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_OK);
	return 1;
}

void* rsxMemalign(u32 alignment,u32 size)
{
	if(!__rsxheap_initialized) {
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_UNINIT);
		return NULL;
	}
	void *p = heapAllocateAligned(&__rsx_heap,size,alignment);
	if (p == NULL)
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_ALLOCATION);
	else
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_OK);
	return p;
}

void* rsxMalloc(u32 size)
{
	if(!__rsxheap_initialized) {
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_UNINIT);
		return NULL;
	}
	void *p = heapAllocate(&__rsx_heap,size);
	if (p == NULL)
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_ALLOCATION);
	else
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_OK);
	return p;
}

void rsxFree(void *ptr)
{
	/* C99 7.22.3 free(NULL) is a no-op.  Match that: rsxFree(NULL)
	 * succeeds silently (records OK) and does not reach the heap, so a
	 * program that defensively frees an uninitialised or already-freed
	 * pointer -- the same idiom it applies to free -- keeps working.
	 * Interior and out-of-range pointers are still rejections and are
	 * recorded as PS3TC_RSX_MM_LASTERROR_FREE. */
	if (ptr == NULL) {
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_OK);
		return;
	}
	if(!__rsxheap_initialized) {
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_UNINIT);
		return;
	}
	if (!heapFree(&__rsx_heap,ptr))
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_FREE);
	else
		__rsx_mm_set_last_error(PS3TC_RSX_MM_LASTERROR_OK);
}

/* ---- diagnostics (see <rsx/ps3tc_mm_lasterror.h>) ------------------ */

s32 ps3tc_rsx_mm_last_error(void)
{
	return __atomic_load_n(&__rsx_mm_last_error, __ATOMIC_RELAXED);
}

const char *ps3tc_rsx_mm_last_error_string(s32 code)
{
	switch (code) {
	case PS3TC_RSX_MM_LASTERROR_OK:
		return "most recent RSX heap call succeeded (or no failure yet)";
	case PS3TC_RSX_MM_LASTERROR_GCM_CONFIG:
		return "gcmGetConfiguration failed during rsxHeapInit";
	case PS3TC_RSX_MM_LASTERROR_UNINIT:
		return "RSX heap not initialised (call rsxHeapInit first)";
	case PS3TC_RSX_MM_LASTERROR_ALLOCATION:
		return "RSX allocation failed (heap exhausted or request too large)";
	case PS3TC_RSX_MM_LASTERROR_FREE:
		return "rsxFree rejected the pointer (interior or out of range)";
	default:
		return "unknown last error";
	}
}
