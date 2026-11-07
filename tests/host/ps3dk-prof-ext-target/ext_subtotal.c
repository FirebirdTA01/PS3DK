/* The heterogeneous-workload sample's EXTENSION subtotal in one PPU data model (lp64-ext port, design 2.2 and 5.2 L3):
 * compiled, never linked or run.  It recomputes, as constant expressions, the extension regions ps3dk_prof_init_ex
 * carves (prof_core.c, the PS3DK_PROF_WITH_SPU block after the v1 regions: each take() rounds count * elem up to 128
 * bytes) for the sample's configuration (prof_ext_bind.c: max_threads 1, PE_MAX_WORKERS lanes of PE_RING_SLOTS
 * slots, a PE_IMAGE_BYTES image arena, one GPU domain of PE_GPU_REPORT_COUNT report slots), with this data model's
 * real structure sizes, and asserts that it fits PE_MEM_MAX, the sample's spu_mem_max.  The subtotal and the margin
 * are the sizes of two arrays (nm -S); the driver prints both for each data model.  A model, not a run: if the
 * runtime's layout changes, this file must change with it (the init block code is the reference).
 * The build declares the data model (-DEXT_SUB_LP64=0|1), bound to the compiled pointer model. */
#include <stddef.h>
#include <stdint.h>

#include "prof_internal.h"
#include "gpu_internal.h"
#include "prg.h"
#include "psp_layout.h"
#include "psp_ppu.h"

#include "prof_ext.h" /* the sample's PE_* configuration */

#ifndef EXT_SUB_LP64
#error "EXT_SUB_LP64 must be defined by the build (0 = ILP32, 1 = LP64)"
#endif
#ifdef __LP64__
_Static_assert(EXT_SUB_LP64 == 1 && sizeof(void *) == 8, "ext_subtotal: LP64 declared and compiled");
#else
_Static_assert(EXT_SUB_LP64 == 0 && sizeof(void *) == 4, "ext_subtotal: ILP32 declared and compiled");
#endif

/* the cap the subtotal must fit: the sample's PE_MEM_MAX (EXT_SUB_CAP only for the driver's control that the fit
 * assertion can fail) */
#ifndef EXT_SUB_CAP
#define EXT_SUB_CAP ((uint64_t)PE_MEM_MAX)
#endif
#define SUB_R128(b) ((((uint64_t)(b)) + 127u) & ~(uint64_t)127u)
#define SUB_THREADS 1u
#define SUB_LANES ((uint64_t)PE_MAX_WORKERS)
#define SUB_GPU 1u
#define SUB_GPU_SLOTS ((uint64_t)PE_GPU_REPORT_COUNT)
#define SUB_SPU                                                                                                       \
    (SUB_R128(sizeof(psp_ppu)) + SUB_R128(PSP_SCRATCH_BYTES) + SUB_R128((uint64_t)PE_RING_SLOTS * PSP_SLOT) +         \
     SUB_R128(PE_IMAGE_BYTES) + SUB_R128(SUB_LANES * ((uint64_t)PE_RING_SLOTS * PSP_SLOT + PSP_CTL_BYTES)))
#define SUB_ACC SUB_R128((uint64_t)SUB_THREADS * sizeof(ext_acc))
#define SUB_GPU_BYTES                                                                                                 \
    (SUB_R128((uint64_t)SUB_GPU * sizeof(gpu_slot)) + SUB_R128(sizeof(prg_ledger)) +                                  \
     SUB_R128((uint64_t)SUB_GPU * SUB_GPU_SLOTS * sizeof(prg_meta)) +                                                 \
     SUB_R128((uint64_t)SUB_GPU * SUB_GPU_SLOTS * sizeof(prg_stage)) +                                                \
     SUB_R128((uint64_t)SUB_GPU * PRG_SCRATCH_FOR(SUB_GPU_SLOTS)))
#define SUB_TOTAL (SUB_SPU + SUB_ACC + SUB_GPU_BYTES)

_Static_assert(PE_RING_SLOTS == 1024u && PE_IMAGE_BYTES == 262144u && PE_MAX_WORKERS == 6u &&
                   PE_GPU_REPORT_COUNT == 32u,
               "ext_subtotal: the sample configuration this model was written for");
_Static_assert(SUB_TOTAL <= (uint64_t)EXT_SUB_CAP, "ext_subtotal: the sample's extension subtotal fits PE_MEM_MAX");

char ext_sub_total[SUB_TOTAL];
char ext_sub_margin[(uint64_t)EXT_SUB_CAP - SUB_TOTAL + 1u]; /* margin + 1 (never zero-sized) */
char ext_sub_spu[SUB_SPU];
char ext_sub_gpu[SUB_GPU_BYTES];
char ext_sub_sizeof_psp_ppu[sizeof(psp_ppu)];
char ext_sub_sizeof_gpu_slot[sizeof(gpu_slot)];
char ext_sub_sizeof_prg_meta[sizeof(prg_meta)];
char ext_sub_sizeof_prg_stage[sizeof(prg_stage)];
char ext_sub_sizeof_prg_ledger[sizeof(prg_ledger)];
char ext_sub_sizeof_ext_acc[sizeof(ext_acc)];
