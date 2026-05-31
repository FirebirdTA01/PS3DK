/* SPU hole-fill -- RPCS3 HLE probe.
 * Fills hole with NOPs via libspuPSGL, patches JTS last.
 */
#include <PSGL/spu_psgl.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stddef.h>
#include <sys/spu_thread.h>

typedef struct {
    unsigned long long initAddr, holeEA;
    uint32_t holeSizeInWord;
} __attribute__((aligned(128))) PSGLholeParams;

static unsigned int g_ls[4096] __attribute__((aligned(128)));

int main(unsigned long long arg1, unsigned long long arg2,
         unsigned long long arg3, unsigned long long arg4)
{
    PSGLholeParams params;
    unsigned int i;

    (void)arg2; (void)arg3; (void)arg4;

    spu_mfcdma64(&params, (unsigned int)(arg1 >> 32),
                 (unsigned int)(arg1 & 0xffffffffULL),
                 sizeof(params), 5u, 0x40);
    spu_writech(MFC_WrTagMask, 1u << 5u);
    (void)spu_mfcstat(2);

    psglSPUInit(params.initAddr);

    if (params.holeSizeInWord < 2u || params.holeSizeInWord > 4096u)
        sys_spu_thread_exit(1);

    for (i = 0; i < params.holeSizeInWord; i++)
        g_ls[i] = 0u;

    /* body words 1..N-1 */
    if (params.holeSizeInWord > 1u) {
        psglSPUWriteMappedBuffer(params.holeEA + 4u, g_ls + 1,
                                  (params.holeSizeInWord - 1u) * 4u);
        spu_writech(MFC_WrTagMask, 1u << PSGL_WRITE_TAG);
        (void)spu_mfcstat(2);
    }

    /* word 0 last (JTS patch) */
    psglSPUWriteMappedBuffer(params.holeEA, g_ls, 4u);
    spu_writech(MFC_WrTagMask, 1u << PSGL_WRITE_TAG);
    (void)spu_mfcstat(2);

    sys_spu_thread_exit(0);
    return 0;
}
