/*! \file cell/sysutil_game_internal.h
 \brief Internal-suffix sysutil game lifecycle helpers exported by
        the cellSysutil PRX:

  - cellSysutilGameExit_I       — graceful exit, returns to XMB
  - cellSysutilGamePowerOff_I   — schedule console power-off
  - cellSysutilGameReboot_I     — schedule console reboot
  - cellSysutilGameDataExit     — exit code path for game-data flow
  - cellSysutilGameDataAssignVmc — VMC (virtual-memory-card) bind for
                                   PSP/PS1 emu; takes no args

  Not in the 475-era reference headers — the trailing `_I` suffix
  marks them as engine-internal.  Reverse-engineered signatures
  cross-checked against RPCS3 (rpcs3/Emu/Cell/Modules/cellSysutil.cpp).
  All five resolve via libsysutil_stub.a.
*/

#ifndef PS3TC_CELL_SYSUTIL_GAME_INTERNAL_H
#define PS3TC_CELL_SYSUTIL_GAME_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int cellSysutilGameExit_I(void);
extern int cellSysutilGamePowerOff_I(void);
extern int cellSysutilGameReboot_I(void);

/* RPCS3 reverse: takes one u32 of unspecified meaning (likely a
 * mode flag — 0 = normal exit, non-zero = forced).  Returns int. */
extern int cellSysutilGameDataExit(uint32_t mode);

/* No documented arguments; RPCS3 records cellSysutilGameDataAssignVmc()
 * with zero parameters.  Likely picks up VMC state from the active
 * cellSaveData session set up beforehand. */
extern int cellSysutilGameDataAssignVmc(void);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_GAME_INTERNAL_H */
