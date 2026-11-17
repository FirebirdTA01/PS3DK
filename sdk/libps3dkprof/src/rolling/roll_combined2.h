/* Private schema2 accepted-state boundary. Stage A: FIFO/43 plus existing
 * PPU/WINDOWS/SPU; AUTO remains refused. No runtime/public enablement. */
#ifndef PS3DK_PROF_ROLL_COMBINED2_H
#define PS3DK_PROF_ROLL_COMBINED2_H
#include "roll_combined.h"
#ifdef __cplusplus
extern "C" {
#endif
#define pfm2_plan ps3dk_prof__roll_combined2_plan
#define pfm2_init ps3dk_prof__roll_combined2_init
#define pfm2_prepare ps3dk_prof__roll_combined2_prepare
#define pfm2_work ps3dk_prof__roll_combined2_work
typedef struct {
    pfm_config shared;
    uint32_t rsx_domains, windows, fifo_membership_runs, accounting_groups;
} pfm2_config;
typedef pfm_layout pfm2_layout;
/* All capacities explicit. Checked plan includes all pools, replacement scratch
 * and canonical checkpoint bound. One returned arena size; caller supplies two.
 * Only actual input/scans/serialization consume per-prepare work, not historical
 * census multiplicities. Context/accepted/input remain immutable on refusal. */
int pfm2_plan(const pfm2_config *, pfm2_layout *);
int pfm2_init(void *, size_t, const pfm2_config *, const uint8_t[16],
              void *, size_t, void *, size_t, size_t *);
int pfm2_prepare(void *, const void *, void *, const void *, size_t,
                 void *, size_t, pfo_prepared *);
/* Same checked reservation consumed by prepare, independent of accepted history.
 * On failure both outputs remain unchanged; sum is checked before admission. */
int pfm2_work(const void *, const void *, size_t, uint64_t *core_work, uint64_t *family_work);
#ifdef __cplusplus
}
#endif
#endif
