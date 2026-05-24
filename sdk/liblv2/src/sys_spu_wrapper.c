/*
 * liblv2 — sysSpuImageOpenELF wrapper (clean-room).
 *
 * Behavioral contract (re-derived, not copied):
 *   sysSpuImageImport (SPRX import, sysPrxForUser, alias of
 *   sys_spu_image_import) consumes an in-memory SPU ELF.  The
 *   "OpenELF" convenience entry point reads the named file fully into
 *   a heap buffer, imports it in PROTECT mode, then releases the
 *   buffer (the kernel copies what it needs during import, so the
 *   transient buffer is safe to free immediately after).
 *
 *   Failure policy: any I/O or allocation failure yields -1 with all
 *   intermediate resources released; the image struct is left
 *   untouched on failure.  Success returns sysSpuImageImport's status.
 */

/* SDK headers first (they pull the legacy <ppu-asm.h>), our guarded
 * <sys/ppu-asm.h> last — see thread_wrapper.c for the rationale. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/spu.h>
#include <sys/ppu-asm.h>

s32 sysSpuImageOpenELF(sysSpuImage *image, const char *path)
{
    FILE  *f;
    long   sz;
    void  *buf;
    size_t got;
    s32    ret = -1;

    f = fopen(path, "rb");
    if (!f)
        return -1;

    if (fseek(f, 0, SEEK_END) != 0)
        goto out_close;
    sz = ftell(f);
    if (sz <= 0)
        goto out_close;
    if (fseek(f, 0, SEEK_SET) != 0)
        goto out_close;

    buf = malloc((size_t)sz);
    if (!buf)
        goto out_close;

    got = fread(buf, 1, (size_t)sz, f);
    if (got == (size_t)sz)
        ret = sysSpuImageImport(image, buf, SPU_IMAGE_PROTECT);

    free(buf);

out_close:
    fclose(f);
    return ret;
}
