/* libfiber_stub_extras: cellFiberPpuInitialize and the fiber TLS area.
 *
 * cellFiberPpuInitialize hands the libfiber SPRX a 64-byte area of
 * THREAD-LOCAL storage, _gCellFiberPpuThreadLocalStorage: the SPRX keeps
 * each PPU thread's fiber state there and finds it through the thread
 * pointer (r13) in every thread that runs fibers, so the area must be in
 * the program's TLS block (.tbss), not ordinary .bss.
 *
 * The SPRX must already be loaded (cellSysmoduleLoadModule(CELL_SYSMODULE_FIBER)):
 *   not loaded        -> CELL_FIBER_ERROR_STAT
 *   other query error -> that error
 *   loaded            -> _cellFiberPpuInitialize(area), CELL_OK (its own
 *                        result is private to the module)
 *
 * This translation unit gets ar-appended to libfiber_stub.a after the nidgen
 * stub generation step (see scripts/build-cell-stub-archives.sh).  It
 * declares the two sysmodule names it needs rather than including
 * <cell/sysmodule.h>, whose <sys/memory.h> dependency needs LV2_SYSCALL.
 */

#include <stdint.h>
#include <cell/error.h>

#ifndef CELL_SYSMODULE_FIBER
#define CELL_SYSMODULE_FIBER            0x0043
#endif
#define SYSMODULE_ERROR_UNLOADED        ((int)0x80012003)
#define CELL_FIBER_ERROR_STAT_LOCAL     ((int)0x8076000f)

extern int cellSysmoduleIsLoaded(unsigned int moduleId);

#include <cell/fiber/ppu_initialize.h>

__thread unsigned char _gCellFiberPpuThreadLocalStorage[64] __attribute__((aligned(4)));

/* Called from the assembly wrapper in fiber_init_wrapper.S, which exports
 * the function descriptor and dot symbol. */
int _fiberPpuInitializeImpl(void)
{
    int rc = cellSysmoduleIsLoaded(CELL_SYSMODULE_FIBER);

    if (rc == SYSMODULE_ERROR_UNLOADED)
        return CELL_FIBER_ERROR_STAT_LOCAL;
    if (rc != 0)
        return rc;
    (void)_cellFiberPpuInitialize(_gCellFiberPpuThreadLocalStorage);
    return 0;
}
