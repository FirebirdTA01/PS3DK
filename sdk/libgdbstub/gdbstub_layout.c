/* gdbstub_layout.c - pinned register layout (real data, not core behavior).
 *
 * This file is INDEPENDENT of the core and of the stub: it holds the
 * constant 71-register description and the width table. It must remain
 * byte-stable when gdbstub_stub.c (and later the real core) is replaced.
 * Its constants match register-layout.json (71 registers, 556 bytes,
 * 1112 hex chars). red_test.c cross-checks the stub's layout queries
 * against the embedded copy of that table.
 *
 * Order: r0..r31, f0..f31, pc, msr, cr, lr, ctr, xer, fpscr.
 * Widths: 68 x 64-bit, then cr=32, xer=32, fpscr=32.
 */
#include "gdbstub.h"

unsigned gdbstub_reg_count(void)
{
  return GDBSTUB_REG_COUNT;
}

unsigned gdbstub_reg_bytes(unsigned reg)
{
  if (reg >= GDBSTUB_REG_COUNT)
    return 0;
  /* cr=66, xer=69, fpscr=70 are 32-bit; everything else is 64-bit. */
  if (reg == 66u || reg == 69u || reg == 70u)
    return 4u;
  return 8u;
}

unsigned gdbstub_reg_total_bytes(void)
{
  return GDBSTUB_REG_TOTAL_BYTES;
}

unsigned gdbstub_reg_total_hex(void)
{
  return GDBSTUB_REG_TOTAL_HEX;
}
