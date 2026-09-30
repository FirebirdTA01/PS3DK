/* hello-raw-spu, SPU side: read n from the inbound mailbox, answer 3n + 1
 * on the outbound mailbox, then stop with code 0x123 for the PPU to see in
 * SPU_Status. */
#include <stdlib.h>      /* brings in the SPU intrinsics, as the SDK stdlib.h does */
#include <spu_mfcio.h>

int main(void)
{
    unsigned int n = spu_read_in_mbox();
    spu_write_out_mbox(3 * n + 1);
    spu_stop(0x123);
    return 0;
}
