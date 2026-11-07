/* The extension's data-model contract (lp64-ext port, design section 5.2 L1): compiled, never linked or run, three
 * ways (the host compiler for the public subset, the PPU compiler for ILP32, and with -mlp64 for LP64); every way must
 * compile.  The build DECLARES the data model it means (-DEXT_ABI_LP64=0|1), and the first assertion binds that
 * declaration to the compiled pointer model, so a probe built for the wrong ABI fails instead of checking the other
 * ABI's numbers.
 *
 * ABI-INDEPENDENT: every structure the SPU program, the file or the sample's SPU arguments see has the same size and
 * offsets in both data models.  ABI-DEPENDENT: struct ps3dk_prof_spu_desc (two pointers: 16 bytes ILP32, 24 LP64)
 * and the pointer width.  Target only: CellGcmContextData keeps four 32-bit fields (mode(SI)) in both data models,
 * which is what makes the GCM binding's narrowing store of current lossless (gpu_port_gcm.c), and the report entry
 * layout.
 *
 * Mutation controls (-DEXT_ABI_MUT=<n>; each must fail to compile with its named assertion, judged by the driver):
 *   1 lane_order   a lane mirror with ea_ctl before ea_ring                   killed in both data models
 *   2 ctx_width    a context mirror with native pointers instead of mode(SI)  killed in LP64; EQUIVALENT in ILP32
 *                                                                             (native pointers are 4 bytes there)
 *   3 desc_lp64    the ILP32 descriptor size expected under -mlp64            killed in LP64; EQUIVALENT in ILP32
 *   4 desc_ilp32   the LP64 descriptor size expected in ILP32                 killed in ILP32; EQUIVALENT in LP64
 *   5 abi_decl     the opposite data model declared                           killed in both data models (host too)
 * An EQUIVALENT mutant is the unmodified contract in that data model: it must compile there. */
#include <stddef.h>
#include <stdint.h>

#include "prof_gpu.h"
#include "prof_spu.h"
#include "psp_layout.h"

#include "prof_ext.h" /* the sample's SPU argument words (pe_args) */
#include "spu_pool.h" /* the sample's work slot */

#ifdef EXT_ABI_TARGET
#include <cell/gcm.h>
#endif

#ifndef EXT_ABI_LP64
#error "EXT_ABI_LP64 must be defined by the build (0 = ILP32, 1 = LP64)"
#endif
#ifndef EXT_ABI_MUT
#define EXT_ABI_MUT 0
#endif

#if EXT_ABI_MUT == 5
#define EXT_ABI_DECLARED (!EXT_ABI_LP64)
#else
#define EXT_ABI_DECLARED EXT_ABI_LP64
#endif
#ifdef __LP64__
#define EXT_ABI_COMPILED 1
#else
#define EXT_ABI_COMPILED 0
#endif
_Static_assert(EXT_ABI_DECLARED == EXT_ABI_COMPILED && sizeof(void *) == (EXT_ABI_DECLARED ? 8u : 4u) &&
                   sizeof(uintptr_t) == sizeof(void *) && sizeof(size_t) == sizeof(void *),
               "ext_abi: the declared data model is the compiled one");

/* ---- ABI-independent ---- */
_Static_assert(sizeof(ps3dk_prof_ext_config) == 40u, "ext_abi: ps3dk_prof_ext_config 40 bytes");
_Static_assert(offsetof(ps3dk_prof_ext_config, spu_mem_max) == 16u && offsetof(ps3dk_prof_ext_config, reserved) == 32u,
               "ext_abi: ps3dk_prof_ext_config offsets");

#if EXT_ABI_MUT == 1
typedef struct {
    uint32_t handle, ring_slots;
    uint64_t ea_ctl, ea_ring;
} ext_abi_lane;
#else
typedef ps3dk_prof_spu_lane ext_abi_lane;
#endif
_Static_assert(sizeof(ext_abi_lane) == 24u && offsetof(ext_abi_lane, handle) == 0u &&
                   offsetof(ext_abi_lane, ring_slots) == 4u && offsetof(ext_abi_lane, ea_ring) == 8u &&
                   offsetof(ext_abi_lane, ea_ctl) == 16u,
               "ext_abi: ps3dk_prof_spu_lane 24 bytes at 0/4/8/16");

_Static_assert(sizeof(ps3dk_prof_spu_lane_stats) == 88u, "ext_abi: ps3dk_prof_spu_lane_stats 88 bytes");
_Static_assert(offsetof(ps3dk_prof_spu_lane_stats, final_written) == 0u &&
                   offsetof(ps3dk_prof_spu_lane_stats, fin_state) == 4u &&
                   offsetof(ps3dk_prof_spu_lane_stats, next_seq) == 8u &&
                   offsetof(ps3dk_prof_spu_lane_stats, events) == 16u &&
                   offsetof(ps3dk_prof_spu_lane_stats, lost) == 24u &&
                   offsetof(ps3dk_prof_spu_lane_stats, have_post_join) == 32u &&
                   offsetof(ps3dk_prof_spu_lane_stats, post_state) == 36u &&
                   offsetof(ps3dk_prof_spu_lane_stats, post_next_seq) == 40u &&
                   offsetof(ps3dk_prof_spu_lane_stats, post_older_lost) == 48u &&
                   offsetof(ps3dk_prof_spu_lane_stats, post_refused) == 56u &&
                   offsetof(ps3dk_prof_spu_lane_stats, stale) == 64u &&
                   offsetof(ps3dk_prof_spu_lane_stats, cancelled) == 72u &&
                   offsetof(ps3dk_prof_spu_lane_stats, unstable) == 80u,
               "ext_abi: ps3dk_prof_spu_lane_stats offsets");

_Static_assert(sizeof(ps3dk_prof_gpu_zone) == 24u && offsetof(ps3dk_prof_gpu_zone, gpu) == 0u &&
                   offsetof(ps3dk_prof_gpu_zone, depth) == 4u && offsetof(ps3dk_prof_gpu_zone, begin) == 8u &&
                   offsetof(ps3dk_prof_gpu_zone, desc) == 16u && offsetof(ps3dk_prof_gpu_zone, pad) == 20u,
               "ext_abi: ps3dk_prof_gpu_zone 24 bytes at 0/4/8/16/20");
_Static_assert(sizeof(ps3dk_prof_gpu_stats) == 160u && offsetof(ps3dk_prof_gpu_stats, refused_api) == 0u &&
                   offsetof(ps3dk_prof_gpu_stats, cal_bad) == 80u && offsetof(ps3dk_prof_gpu_stats, events) == 88u &&
                   offsetof(ps3dk_prof_gpu_stats, brackets_dropped) == 120u &&
                   offsetof(ps3dk_prof_gpu_stats, retired) == 128u && offsetof(ps3dk_prof_gpu_stats, pad) == 156u,
               "ext_abi: ps3dk_prof_gpu_stats 160 bytes");

_Static_assert(sizeof(pe_args) == 32u && offsetof(pe_args, arg3) == 24u, "ext_abi: the sample's pe_args 32 bytes");
_Static_assert(sizeof(work_slot) == 256u && offsetof(work_slot, sequence) == 128u,
               "ext_abi: the sample's work_slot 256 bytes, sequence on its own line");

_Static_assert(PSP_CTL_BYTES == 256u && PSP_B == 128u && PSP_A_READ_IDX == 0u && PSP_A_CAL_REQ == 8u &&
                   PSP_A_CAL_CANCEL == 10u && PSP_B_GEN == 128u && PSP_B_WRITE_IDX == 136u &&
                   PSP_B_NEXT_SEQ == 144u && PSP_B_OLDER_LOST == 152u && PSP_B_REFUSED == 160u &&
                   PSP_B_REFUSED_CALLS == 168u && PSP_B_STATE == 176u && PSP_B_FLAGS == 180u &&
                   PSP_B_CAL_ANSWERED == 184u && PSP_B_EPOCHS_WRITTEN == 188u && PSP_B_GEN_END == 248u &&
                   PSP_SLOT == 8u,
               "ext_abi: the control-block layout constants");

/* ---- ABI-dependent: one expectation per data model ---- */
#if EXT_ABI_MUT == 3
#define EXT_ABI_DESC_BYTES 16u /* the ILP32 size, whatever the data model */
#elif EXT_ABI_MUT == 4
#define EXT_ABI_DESC_BYTES 24u /* the LP64 size, whatever the data model */
#else
#define EXT_ABI_DESC_BYTES (EXT_ABI_COMPILED ? 24u : 16u)
#endif
_Static_assert(sizeof(struct ps3dk_prof_spu_desc) == EXT_ABI_DESC_BYTES &&
                   offsetof(struct ps3dk_prof_spu_desc, ls_addr) == 0u &&
                   offsetof(struct ps3dk_prof_spu_desc, line) == 4u &&
                   offsetof(struct ps3dk_prof_spu_desc, name) == 8u &&
                   offsetof(struct ps3dk_prof_spu_desc, file) == 8u + sizeof(void *),
               "ext_abi: ps3dk_prof_spu_desc 16 bytes ILP32, 24 bytes LP64, name at 8");

/* ---- target only: the GCM context and report entry ---- */
#ifdef EXT_ABI_TARGET
#if EXT_ABI_MUT == 2
typedef struct {
    uint32_t *begin, *end, *current;
    int32_t (*callback)(void *context, uint32_t count);
} ext_abi_ctx;
#else
typedef CellGcmContextData ext_abi_ctx;
#endif
_Static_assert(sizeof(ext_abi_ctx) == 16u && offsetof(ext_abi_ctx, begin) == 0u && offsetof(ext_abi_ctx, end) == 4u &&
                   offsetof(ext_abi_ctx, current) == 8u && offsetof(ext_abi_ctx, callback) == 12u &&
                   sizeof(((ext_abi_ctx *)0)->current) == 4u && sizeof(((ext_abi_ctx *)0)->callback) == 4u,
               "ext_abi: CellGcmContextData four 32-bit fields at 0/4/8/12 in both data models");
_Static_assert(sizeof(CellGcmReportData) == 16u && offsetof(CellGcmReportData, timer) == 0u &&
                   offsetof(CellGcmReportData, value) == 8u,
               "ext_abi: CellGcmReportData 16 bytes, timer at 0, value at 8");
#endif

/* the sizes the driver records (nm -S): the ABI-dependent descriptor and the pointer */
char ext_abi_sizeof_spu_desc[sizeof(struct ps3dk_prof_spu_desc)];
char ext_abi_sizeof_ptr[sizeof(void *)];
