/* libspurs SPU module runtime - thin LS-read getters that policy
 * modules + tasks call to discover their execution context.
 *
 * The SPU SPURS kernel keeps two control regions in well-known LS
 * locations.  Tasks run on top of these:
 *
 *   LS 0x1c0 (16 bytes - "module info A")
 *     +0x00..0x07: SpursAddress (uint64_t EA of the PPU CellSpurs)
 *     +0x08..0x0b: CurrentSpuId (uint32_t; 0..numSpus-1)
 *     +0x0c..0x0f: TagId (uint32_t DMA tag for this workload)
 *
 *   LS 0x1d0 (16 bytes - "module info B")
 *     +0x0c..0x0f: WorkloadId (uint32_t)
 *
 *   LS 0x170 (16 bytes - "spu count block")
 *     +0x06: SpuCount (uint8_t; total SPUs in this Spurs instance)
 *
 *   LS 0x2fb0 (task control - already used by spurs_task_runtime.c)
 *     +0x08..0x0b: dispatch_base (LS pointer; per-task service table)
 *
 * Both blocks are filled in by the SPRX kernel before each
 * workload / task starts; reading them is just an LS load.  The
 * reference libspurs.a emits one-or-two-instruction bodies for each
 * getter (lqa + optional rotqbyi + bi $0); GCC's plain C with a
 * volatile cast at a 16-byte-aligned LS address generates the same
 * shape.
 */
#include <stdint.h>

#include <cell/spurs/types.h>

#define SPURS_MOD_INFO_A   0x1c0U   /* SpursAddress / CurrentSpuId / TagId */
#define SPURS_MOD_INFO_B   0x1d0U   /* WorkloadId at +0xc */
#define SPURS_SPU_COUNT_LS 0x170U   /* SpuCount at +0x6 */

/* Helpers - explicit 8/4/1-byte LS reads.  uintptr_t cast first so
 * GCC sees a constant address, not a pointer-to-LS-zero. */
static inline uint64_t _ls_read64(uint32_t addr) {
    return *(const volatile uint64_t *)(uintptr_t)addr;
}
static inline uint32_t _ls_read32(uint32_t addr) {
    return *(const volatile uint32_t *)(uintptr_t)addr;
}
static inline uint8_t _ls_read8(uint32_t addr) {
    return *(const volatile uint8_t *)(uintptr_t)addr;
}

uint64_t cellSpursGetSpursAddress(void)
{
    return _ls_read64(SPURS_MOD_INFO_A + 0x0);
}

uint32_t cellSpursGetCurrentSpuId(void)
{
    return _ls_read32(SPURS_MOD_INFO_A + 0x8);
}

uint32_t cellSpursGetTagId(void)
{
    return _ls_read32(SPURS_MOD_INFO_A + 0xc);
}

CellSpursWorkloadId cellSpursGetWorkloadId(void)
{
    return _ls_read32(SPURS_MOD_INFO_B + 0xc);
}

uint32_t cellSpursGetSpuCount(void)
{
    /* Reference reads byte 0x176 and zero-extends to uint32_t. */
    return (uint32_t)_ls_read8(SPURS_SPU_COUNT_LS + 0x6);
}
