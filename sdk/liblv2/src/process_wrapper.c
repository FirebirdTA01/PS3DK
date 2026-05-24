/*
 * liblv2 — sysProcessExitSpawn2 wrapper (clean-room).
 *
 * Behavioral contract (re-derived, not copied):
 *   The SPRX-exported primitive sys_game_process_exitspawn2
 *   (alias sysProcessExitSpawn2Ex, sysPrxForUser NID 0x67f9fedb)
 *   expects the argv / envp vectors as NULL-terminated arrays of
 *   32-bit effective addresses — each element is a string EA, not a
 *   native pointer (docs/abi/cellos-lv2-abi-spec.md §4: pointers that
 *   cross the SPRX boundary are 32-bit EAs).
 *
 *   Under the default ILP32 ABI a `const char *` is already 4 bytes,
 *   so a plain `const char *argv[]` is bit-compatible with the
 *   32-bit-element vector.  Under -mlp64 each element is 8 bytes and
 *   MUST be repacked into a 32-bit-element array before the call, or
 *   the SPRX side reads every other word as garbage.  We rebuild the
 *   vectors unconditionally with ATTRIBUTE_PRXPTR (mode(SI)) elements
 *   so the code is correct for both pointer models without an #ifdef.
 *
 *   sysProcessExitSpawn2Ex does not return on success (it replaces the
 *   process image); the malloc'd shadow vectors are intentionally not
 *   freed for that reason.
 */

/* SDK headers first (they pull the legacy <ppu-asm.h>), our guarded
 * <sys/ppu-asm.h> last — see thread_wrapper.c for the rationale. */
#include <stdlib.h>
#include <lv2/process.h>
#include <sys/ppu-asm.h>

/* SPRX import (resolved from liblv2_stub.a at link time). */
extern void sysProcessExitSpawn2Ex(const char *path,
                                   const char *ATTRIBUTE_PRXPTR argv[],
                                   const char *ATTRIBUTE_PRXPTR envp[],
                                   void *data,
                                   size_t size,
                                   int priority,
                                   u64 flags);

/* Count a NULL-terminated pointer vector (excludes the terminator). */
static int vec_len(const char *const v[])
{
    int n = 0;
    if (v)
        while (v[n])
            ++n;
    return n;
}

/* Repack a native-pointer vector into a 32-bit-EA vector.
 * Returns NULL when the input is NULL (the SPRX accepts NULL for
 * "no argv/envp"). */
static const char *ATTRIBUTE_PRXPTR *
narrow_vec(const char *const src[])
{
    int n, i;
    const char *ATTRIBUTE_PRXPTR *dst;

    if (!src)
        return NULL;

    n = vec_len(src);
    /* one slot per entry plus the NULL terminator; each slot is a
     * 32-bit EA (sizeof a mode(SI) pointer). */
    dst = (const char *ATTRIBUTE_PRXPTR *)
              malloc(sizeof(*dst) * (size_t)(n + 1));
    if (!dst)
        return NULL;

    for (i = 0; i < n; ++i)
        dst[i] = src[i];      /* native ptr -> 32-bit EA (mode(SI)) */
    dst[n] = NULL;
    return dst;
}

void sysProcessExitSpawn2(const char *path,
                          const char *argv[],
                          const char *envp[],
                          void *data,
                          size_t size,
                          int priority,
                          u64 flags)
{
    const char *ATTRIBUTE_PRXPTR *argv32 =
        narrow_vec((const char *const *)argv);
    const char *ATTRIBUTE_PRXPTR *envp32 =
        narrow_vec((const char *const *)envp);

    sysProcessExitSpawn2Ex(path, argv32, envp32,
                           data, size, priority, flags);
    /* not reached on success */
}
