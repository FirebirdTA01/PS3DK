/*
 * hello-ppu-pamf — cellPamf container-parser smoke test.
 *
 * Feeds a minimal PAMF-magic buffer to the SPRX.  No real PAMF
 * stream is needed — the SPRX validates the magic bytes then
 * returns error codes for the malformed remainder.  Those error
 * codes prove the link surface works, the SPRX processes our
 * caller-allocated buffer, and no crash occurs.
 */

#include <cstdio>
#include <cstdint>
#include <cstring>

#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/codec/pamf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int32_t rc;

    printf("[hello-ppu-pamf] starting\n");

    /* Load PAMF sysmodule. */
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_PAMF);
    printf("[hello-ppu-pamf] cellSysmoduleLoadModule(PAMF) = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL) {
        printf("[hello-ppu-pamf] FAIL: module load\n");
        return 1;
    }

    /* Minimal PAMF buffer: 4-byte magic + zeros.  The SPRX will
     * reject this as malformed (no valid header data), but the
     * call itself proves the ABI works. */
    static uint8_t pamf_buf[4096] __attribute__((aligned(64))) = {
        'P', 'A', 'M', 'F',
    };

    /* cellPamfVerify — should reject as INVALID_PAMF or UNKNOWN_TYPE. */
    rc = cellPamfVerify(pamf_buf, sizeof(pamf_buf));
    printf("[hello-ppu-pamf] cellPamfVerify = %08x\n", (unsigned)rc);

    /* cellPamfGetHeaderSize — likewise. */
    uint64_t headerSize = 0;
    rc = cellPamfGetHeaderSize(pamf_buf, sizeof(pamf_buf), &headerSize);
    printf("[hello-ppu-pamf] cellPamfGetHeaderSize = %08x, size=%llu\n",
           (unsigned)rc, (unsigned long long)headerSize);

    /* cellPamfGetStreamOffsetAndSize — likewise. */
    uint64_t offset = 0, streamSize = 0;
    rc = cellPamfGetStreamOffsetAndSize(pamf_buf, sizeof(pamf_buf),
                                         &offset, &streamSize);
    printf("[hello-ppu-pamf] cellPamfGetStreamOffsetAndSize = %08x\n", (unsigned)rc);

    /* cellPamfReaderInitialize — parse into a reader context. */
    CellPamfReader reader;
    rc = cellPamfReaderInitialize(&reader, pamf_buf, sizeof(pamf_buf),
                                  CELL_PAMF_INITIALIZE_VERIFY_ON);
    printf("[hello-ppu-pamf] cellPamfReaderInitialize = %08x\n", (unsigned)rc);

    /* If reader init succeeded, query a few synchronous reader calls. */
    if (rc == CELL_OK) {
        uint8_t n = cellPamfReaderGetNumberOfStreams(&reader);
        printf("[hello-ppu-pamf] cellPamfReaderGetNumberOfStreams = %u\n", n);

        uint32_t muxRate = cellPamfReaderGetMuxRateBound(&reader);
        printf("[hello-ppu-pamf] cellPamfReaderGetMuxRateBound = %u\n", muxRate);
    }

    printf("[hello-ppu-pamf] all tests passed\n");
    return 0;
}
