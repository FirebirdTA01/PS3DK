/* HOST EMULATION of the SPU lane premises (design p3.2 r3 F, integration r2 section 7, I2 delta section 5).
 * Not target evidence.  On the target, line B is a putlluc store (indivisible, by the stated premise), line A is
 * read with getllar/putllc, and the PPU uses plain aligned loads/stores with barriers.  On the host, C11 threads
 * need explicit happens-before edges, so:
 *   - every access to a 128-byte control line (SPU putlluc / getllar / putllc; PPU line-B loads and line-A stores)
 *     takes that line's emulation mutex (striped by address);
 *   - line A carries a version, bumped by every PPU store to it; getllar records it, and putllc stores ONLY if it is
 *     unchanged (else it reports LOST and writes nothing), so an intervening PPU store is never overwritten;
 *   - ring data the SPU put before its putlluc happens-before any PPU ring load that follows a line-B load which saw
 *     that version (the line mutex is the release/acquire pair); the PPU's ring loads before its read_idx store
 *     happen-before the SPU's later overwrite, which follows its line-A acquire. */
#ifndef PSP_HOST_EMUL_H
#define PSP_HOST_EMUL_H

#include <stdint.h>

#define psp_emul_line_lock ps3dk_prof__psp_emul_line_lock
#define psp_emul_line_unlock ps3dk_prof__psp_emul_line_unlock
#define psp_emul_version ps3dk_prof__psp_emul_version
#define psp_emul_version_bump ps3dk_prof__psp_emul_version_bump
#define psp_emul_drop_line_a ps3dk_prof__psp_emul_drop_line_a
#define psp_emul_stripe ps3dk_prof__psp_emul_stripe

void psp_emul_line_lock(uint64_t ea);
void psp_emul_line_unlock(uint64_t ea);
/* the stripe owning a line: its mutex AND its version counter (exposed for the collision tests) */
unsigned psp_emul_stripe(uint64_t ea);
/* line A's version (caller holds the line lock, which owns that version) */
uint64_t psp_emul_version(uint64_t ea);
void psp_emul_version_bump(uint64_t ea);
/* TEST ONLY (the TSan negative control): when set, line-A accesses skip the line lock */
extern int psp_emul_drop_line_a;

#endif
