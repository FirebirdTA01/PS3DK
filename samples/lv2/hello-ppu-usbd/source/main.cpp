/*
 * hello-ppu-usbd — libusbd bring-up smoke test.
 *
 * Exercises the PPU libusbd surface end-to-end through the cell-SDK
 * surface in <cell/usbd.h>:
 *
 *   1. cellSysmoduleLoadModule(CELL_SYSMODULE_USBD)
 *   2. cellUsbdInit
 *   3. cellUsbdAllocateMemory / cellUsbdFreeMemory  (round-trip)
 *   4. cellUsbdSetThreadPriority2     (smoke; HLE returns OK or error)
 *   5. cellUsbdEnd
 *
 * RPCS3's cellUsbd HLE has Init/End at warning level (logs and returns
 * CELL_OK), with the rest as UNIMPLEMENTED_FUNC stubs.  The gate here
 * is link-cleanliness + clean boot + Init/End round-trip; full USB I/O
 * needs hardware or a future RPCS3 USB stack.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include <sys/process.h>

#include <cell/sysmodule.h>
#include <cell/usbd.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    std::printf("hello-ppu-usbd: libusbd bring-up smoke test\n");

    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_USBD);
    std::printf("  cellSysmoduleLoadModule(USBD): %#x\n", rc);
    if (rc != CELL_OK && rc != (int)0x80012003 /* already loaded */) {
        std::printf("FAIL: sysmodule load\n");
        return 1;
    }

    rc = cellUsbdInit();
    std::printf("  cellUsbdInit: %#x\n", rc);
    if (rc != CELL_OK) {
        std::printf("FAIL: cellUsbdInit\n");
        return 1;
    }

    void *scratch = nullptr;
    rc = cellUsbdAllocateMemory(&scratch, 256);
    std::printf("  cellUsbdAllocateMemory(256): rc=%#x ptr=%p\n", rc, scratch);

    if (scratch != nullptr) {
        rc = cellUsbdFreeMemory(scratch);
        std::printf("  cellUsbdFreeMemory: %#x\n", rc);
    }

    rc = cellUsbdSetThreadPriority2(/*event*/1500, /*usbd*/1500, /*callback*/1500);
    std::printf("  cellUsbdSetThreadPriority2: %#x\n", rc);

    rc = cellUsbdEnd();
    std::printf("  cellUsbdEnd: %#x\n", rc);

    std::printf("SUCCESS\n");
    return 0;
}
