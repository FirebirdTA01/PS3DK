/* Initialization-only admission of the caller-owned default FIFO extent.
 * No command publication or retirement occurs here. The existing two-phase
 * callback remains responsible for retiring a lap before reusing its storage.
 */
#include <stdint.h>
#include <rsx/gcm_sys.h>
#include <cell/gcm/ps3tc_fifo_wrap.h>

int32_t ps3tc_fifo_init_extent(gcmContextData *ctx, uint32_t command_bytes,
                             uint32_t io_bytes, const void *io_address)
{
    const uint64_t base = (uintptr_t)io_address;
    const uint64_t io_limit = base + io_bytes;
    const uint64_t limit = base + command_bytes;
    uint64_t begin, current, end, address;
    uint32_t base_offset, offset;

    /* Context fields and RSX effective addresses are 32-bit in both ABIs.
     * Calculate exclusive bounds widened, before any pointer arithmetic.
     * Keep at least one reserved prefix word and one tail JUMP word. */
    if (!ctx || !base || base > UINT32_MAX || (base & 3u) ||
        command_bytes < 12u || (command_bytes & 3u) ||
        command_bytes > io_bytes || io_limit > UINT64_C(0x100000000))
        return -1;
    /* Widen the ABI's PRXPTR fields to native pointers first in LP64. */
    begin = (uintptr_t)(void *)ctx->begin;
    current = (uintptr_t)(void *)ctx->current;
    end = (uintptr_t)(void *)ctx->end;
    if (((begin | current | end) & 3u) || begin <= base ||
        begin >= end || current < begin || current > end || end > limit - 4u)
        return -1;

    /* An extent is not proved by its endpoints alone. Check every 4 KiB
     * boundary plus the final word for a contiguous IO translation. This
     * is initialization work, never a per-reserve operation. The command
     * jump encoding uses bit 29, so no address may consume that bit. */
    if (gcmAddressToOffset((const void *)(uintptr_t)base, &base_offset) != 0 ||
        (base_offset & 3u) ||
        (uint64_t)base_offset + command_bytes > UINT64_C(0x20000000))
        return -1;
    address = (base & ~UINT64_C(4095)) + 4096u;
    while (address < limit) {
        if (gcmAddressToOffset((const void *)(uintptr_t)address, &offset) != 0 ||
            (uint64_t)offset != (uint64_t)base_offset + address - base)
            return -1;
        address += 4096u;
    }
    if (gcmAddressToOffset((const void *)(uintptr_t)(limit - 4u), &offset) != 0 ||
        (uint64_t)offset != (uint64_t)base_offset + command_bytes - 4u)
        return -1;

    /* Commit only after every check. Preserve firmware's reserved prefix,
     * current (including rsxInit's queued words), and all command bytes.
     * end is the reserved JUMP word, not an extra emit-capacity word. */
    ctx->end = (uint32_t *)(uintptr_t)(limit - 4u);
    return 0;
}
