/* SPU hole-fill for indexed triangle draw.
 *
 * Calls psglFillCommandBufferHole to emit a real GCM draw-index-array
 * into a local LS buffer, then DMAs it to the hole via
 * psglSPUWriteMappedBuffer (body first, word 0 last for JTS release).
 */
#include <PSGL/spu_psgl.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stddef.h>
#include <sys/spu_thread.h>

/* Must match the PPU-side definition byte-for-byte. */
typedef struct {
    unsigned long long initAddr, holeEA;
    uint32_t holeSizeInWord;
    uint32_t mode;
    int32_t count;
    uint32_t type;
    int32_t isIndexMainMemory;
    uint32_t indexOffset;
} __attribute__((aligned(128))) PSGLholeParams;

/* LS buffer for holding the GCM command stream before DMA to the hole.
   Sized for the maximum practical draw (worst case hole ~64 words). */
static unsigned int g_ls[4096] __attribute__((aligned(128)));

int main(unsigned long long arg1, unsigned long long arg2,
         unsigned long long arg3, unsigned long long arg4)
{
    PSGLholeParams params;
    int rc;

    (void)arg2; (void)arg3; (void)arg4;

    /* DMA params struct from PPU. */
    spu_mfcdma64(&params, (unsigned int)(arg1 >> 32),
                 (unsigned int)(arg1 & 0xffffffffULL),
                 sizeof(params), 5u, 0x40);
    spu_writech(MFC_WrTagMask, 1u << 5u);
    (void)spu_mfcstat(2);

    /* Sanity. */
    if (params.holeSizeInWord < 4u || params.holeSizeInWord > 4096u
        || params.count <= 0 || params.count > 256)
        sys_spu_thread_exit(1);

    /* IOIF window config so psglSPUWriteMappedBuffer can reach the
       command buffer (which lives in RSX-mapped main memory). */
    psglSPUInit(params.initAddr);

    /* Build the GCM draw-index-array command stream into the local LS
       buffer.  The result is patched for 128B-boundary safety and
       NOP-padded to holeSizeInWord. */
    rc = psglFillCommandBufferHole(params.mode, params.count, params.type,
                                    params.isIndexMainMemory,
                                    params.indexOffset,
                                    params.holeSizeInWord, g_ls);
    if (rc != 0)
        sys_spu_thread_exit(2);

    /* DMA body words 1..N-1 to the hole.  Word 0 remains JTS so the
       RSX continues to spin until the final write. */
    if (params.holeSizeInWord > 1u) {
        psglSPUWriteMappedBuffer(params.holeEA + 4u, g_ls + 1,
                                  (params.holeSizeInWord - 1u) * 4u);
        spu_writech(MFC_WrTagMask, 1u << PSGL_WRITE_TAG);
        (void)spu_mfcstat(2);
    }

    /* Word 0 last — JTS patch.  RSX sees the real draw command and
       executes the indexed triangle. */
    psglSPUWriteMappedBuffer(params.holeEA, g_ls, 4u);
    spu_writech(MFC_WrTagMask, 1u << PSGL_WRITE_TAG);
    (void)spu_mfcstat(2);

    sys_spu_thread_exit(0);
    return 0;
}
