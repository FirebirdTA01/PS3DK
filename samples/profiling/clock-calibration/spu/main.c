/* Clock calibration probe, SPU side. For each request (a nonzero sequence in the inbound mailbox) it reads its
 * decrementer at once, reads it again after a fixed spin, DMAs the PAYLOAD line {sequence, dec, dec2, wrote}, waits
 * for that transfer, and only then DMAs the separate COMMIT line {sequence}. The PPU trusts a payload only after
 * seeing its exact commit, so a commit can never be observed ahead of its payload.
 *
 * A request with WRITE_FLAG set first WRITES the decrementer (SPU_WrDec) with WRITE_VALUE, then takes the same two
 * reads; the payload's wrote field carries the value written (0 otherwise). It starts a separate post-write epoch:
 * nothing is fitted across it. The thread runs until the PPU terminates its group. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#define WRITE_FLAG 0x80000000u
#define WRITE_VALUE 0xFFFFFFFFu
#define SPIN 4096u

typedef struct {
    uint32_t sequence;
    uint32_t dec;   /* read right after the request (after the write, for a write request) */
    uint32_t dec2;  /* read again after SPIN iterations */
    uint32_t wrote; /* the value written to SPU_WrDec by this request, or 0 */
    uint32_t pad[28];
} __attribute__((aligned(128))) line;

static line payload, commit;

int main(uint64_t payload_ea, uint64_t commit_ea, uint64_t unused2, uint64_t unused3)
{
    (void)unused2, (void)unused3;
    for (;;) {
        uint32_t sequence = spu_read_in_mbox();
        uint32_t wrote = 0;
        if (sequence & WRITE_FLAG) {
            spu_writech(SPU_WrDec, WRITE_VALUE);
            wrote = WRITE_VALUE;
        }
        uint32_t dec = spu_readch(SPU_RdDec);
        for (volatile uint32_t i = 0; i < SPIN; ++i)
            ;
        uint32_t dec2 = spu_readch(SPU_RdDec);
        payload.sequence = sequence;
        payload.dec = dec;
        payload.dec2 = dec2;
        payload.wrote = wrote;
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
