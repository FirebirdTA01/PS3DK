/* Clock calibration probe, SPU side. For each request (a nonzero sequence in the inbound mailbox) it reads its
 * decrementer at once, DMAs the PAYLOAD line {sequence, decrementer}, waits for that transfer, and only then DMAs
 * the separate COMMIT line {sequence}. The PPU trusts a payload only after seeing its exact commit, so a commit can
 * never be observed ahead of its payload. The thread runs until the PPU terminates its group. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

typedef struct {
    uint32_t sequence;
    uint32_t dec;  /* read right after the request */
    uint32_t dec2; /* read again after SPIN iterations: shows whether the decrementer counts at all */
    uint32_t pad[29];
} __attribute__((aligned(128))) line;

#define SPIN 4096u

static line payload, commit;

int main(uint64_t payload_ea, uint64_t commit_ea, uint64_t unused2, uint64_t unused3)
{
    (void)unused2, (void)unused3;
    for (;;) {
        uint32_t sequence = spu_read_in_mbox();
        uint32_t dec = spu_readch(SPU_RdDec); /* the reading: right after the request arrives */
        for (volatile uint32_t i = 0; i < SPIN; ++i)
            ;
        uint32_t dec2 = spu_readch(SPU_RdDec);
        payload.sequence = sequence;
        payload.dec = dec;
        payload.dec2 = dec2;
        mfc_put(&payload, payload_ea, sizeof payload, 0, 0, 0);
        mfc_write_tag_mask(1);
        mfc_read_tag_status_all(); /* the payload is in main memory before the commit is sent */
        commit.sequence = sequence;
        mfc_put(&commit, commit_ea, sizeof commit, 0, 0, 0);
        mfc_write_tag_mask(1);
        mfc_read_tag_status_all();
    }
    return 0;
}
