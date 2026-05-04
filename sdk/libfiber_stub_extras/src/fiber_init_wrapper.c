/* libfiber_stub_extras: fiber_init_wrapper + TLS area.
 *
 * cellFiberPpuInitialize is the public PPU entry point that loads
 * the libfiber SPRX via cellSysmoduleLoadModule (if not already
 * loaded), then forwards to the SPRX's internal _cellFiberPpuInitialize.
 *
 * The reference archive also ships a BSS symbol
 * _gCellFiberPpuThreadLocalStorage with 64 bytes of thread-local
 * storage reserved by the SPRX.  We replicate it here so the
 * link-editor sees the symbol at the expected size (nm -S of the
 * reference fiber_ppu_tls_area_for_prx.o shows size 0x40).
 *
 * This translation unit gets ar-appended to libfiber_stub.a after
 * the nidgen stub generation step (see scripts/build-cell-stub-archives.sh).
 *
 * We avoid including <cell/sysmodule.h> here because the transitive
 * <sys/memory.h> dependency requires LV2_SYSCALL which introduces
 * ordering friction in standalone compilation units.  Instead we
 * declare only the two sysmodule entry points and constants we need.
 */

#include <stdint.h>
#include <cell/error.h>

/* Forward declarations — resolved by libsysmodule_stub.a at link time. */
#ifndef CELL_SYSMODULE_FIBER
#define CELL_SYSMODULE_FIBER            0x0043
#endif
#ifndef CELL_SYSMODULE_LOADED
#define CELL_SYSMODULE_LOADED           0
#endif
#ifndef CELL_SYSMODULE_ERROR_FATAL
#define CELL_SYSMODULE_ERROR_FATAL      0x800120ff
#endif

extern int cellSysmoduleLoadModule(unsigned int moduleId);

/* SPRX internal init — declared in <cell/fiber/ppu_initialize.h>. */
#include <cell/fiber/ppu_initialize.h>

/* 64-byte TLS area — size matches the reference archive's BSS symbol. */
unsigned char _gCellFiberPpuThreadLocalStorage[64];

/* CELL_FIBER_ERROR_STAT — the sysmodule "already initialized" /
 * "double init" return code.  Module-relative error 0xF in the libfiber
 * range. */
#define CELL_FIBER_ERROR_STAT_LOCAL  ((int)0x8076000f)

/* C-level implementation of the init sequence.  Called from the
 * assembly wrapper in fiber_init_wrapper.S which exports the PPC64
 * ELFv1 function descriptor + dot symbol matching the reference
 * archive's nm shape.
 *
 * Semantics mirror the reference archive's cellFiberPpuInitialize:
 *
 *   - cellSysmoduleLoadModule returns 0 (CELL_OK)        -> call _cellFiberPpuInitialize, return 0
 *   - cellSysmoduleLoadModule returns 0x80012003          -> return CELL_FIBER_ERROR_STAT
 *   - any other non-zero return                           -> propagate it
 *
 * Crucially, _cellFiberPpuInitialize's return value is dropped —
 * the SPRX's internal init result is private to the module. */
int _fiberPpuInitializeImpl(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_FIBER);

    if (rc == 0) {
        (void)_cellFiberPpuInitialize(_gCellFiberPpuThreadLocalStorage);
        return 0;
    }
    if (rc == (int)0x80012003) {
        return CELL_FIBER_ERROR_STAT_LOCAL;
    }
    return rc;
}
