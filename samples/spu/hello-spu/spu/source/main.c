// hello-spu — SPU side.
//
// Runs in the 256 KB Local Store. Receives (ea_of_done_flag, ..., ..., ...)
// via the SPU argument area, sets the flag to a known value via DMA, and exits.
//
// This file builds with spu-elf-gcc (GCC 9.5.0 for now; a future forward-port
// to GCC 12.x is planned).

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

// SPU entry point.  Parameters map directly to the four 64-bit EA
// slots in sysSpuThreadArgument: arg1 = .arg0, arg2 = .arg1, etc.
// (Cell SDK / PSL1GHT convention — first slot lands in r3.)
int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg2; (void)arg3; (void)arg4;

    // PPU passed ptr2ea(&spu_done) in sysSpuThreadArgument.arg0.
    uint64_t done_ea = arg1;

    // Prepare a 16-byte aligned buffer holding the value to write.
    __attribute__((aligned(16))) uint32_t buf[4] = { 0xc0ffee, 0, 0, 0 };

    // DMA-put 16 bytes to the done_ea. Tag 0, no list.
    mfc_put(buf, done_ea, 16, 0, 0, 0);
    mfc_write_tag_mask(1 << 0);
    mfc_read_tag_status_all();

    // Use the LV2 thread-exit syscall instead of `return 0` — the
    // spu-elf default crt0 ends with `stop 0x2000`, which the LV2
    // kernel doesn't recognise and surfaces as a fatal STOP code.
    // sys_spu_thread_exit drops a `stop 0x102` ("user thread exit")
    // that the kernel wires up to sysSpuThreadGroupJoin's status
    // word.  Requires libsputhread on the link line (see CMakeLists).
    sys_spu_thread_exit(0);
}
