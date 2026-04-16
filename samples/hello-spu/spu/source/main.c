// hello-spu — SPU side.
//
// Runs in the 256 KB Local Store. Receives (ea_of_done_flag, ..., ..., ...)
// via the SPU argument area, sets the flag to a known value via DMA, and exits.
//
// This file builds with spu-elf-gcc (GCC 9.5.0 for now, GCC 12.4.0 later once
// Phase 2b forward-port lands).

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

// SPU entry point. Parameters come from sysSpuThreadArgument.
// Only ea (64-bit effective address) values are interesting here.
int main(uint64_t spe_id, uint64_t argp_ea)
{
    (void)spe_id;

    // The PPU passes us the EA of its `spu_done` word via arg0.
    // The sysSpuThreadArgument layout puts arg0 at *argp (not dereferencing
    // through argp, but argp IS arg0 for SPUs started via sysSpuThreadInitialize
    // — cross-check with the Sony ABI docs when we write more complex samples).
    uint64_t done_ea = argp_ea;

    // Prepare a 16-byte aligned buffer holding the value to write.
    __attribute__((aligned(16))) uint32_t buf[4] = { 0xc0ffee, 0, 0, 0 };

    // DMA-put 16 bytes to the done_ea. Tag 0, no list.
    mfc_put(buf, done_ea, 16, 0, 0, 0);
    mfc_write_tag_mask(1 << 0);
    mfc_read_tag_status_all();

    return 0;
}
