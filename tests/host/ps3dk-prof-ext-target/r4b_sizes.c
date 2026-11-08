/* P3.3 R4b target SIZE/ASSERT probe (compiled, never linked or run): the sizes of the extension's structures in one
 * PPU data model, each as the size of an array the nm -S of this object reports, and the GCM layout / constant
 * assertions the GPU binding relies on (review 22:26: in the target fixture).  The build declares the data model
 * (-DR4B_SIZES_LP64=0 for ILP32, 1 for -mlp64); the first assertion binds it to the compiled pointer model. */
#include <stddef.h>
#include <stdint.h>

#include <cell/gcm.h>

#include "prof_internal.h"
#include "gpu_internal.h"
#include "gpu_port_gcm.h"
#include "psp_ppu.h"

#ifndef R4B_SIZES_LP64
#error "R4B_SIZES_LP64 must be defined by the build (0 = ILP32, 1 = LP64)"
#endif
#ifdef __LP64__
_Static_assert(R4B_SIZES_LP64 == 1 && sizeof(void *) == 8 && sizeof(uintptr_t) == 8, "LP64 declared and compiled");
#else
_Static_assert(R4B_SIZES_LP64 == 0 && sizeof(void *) == 4 && sizeof(uintptr_t) == 4, "ILP32 declared and compiled");
#endif
_Static_assert(sizeof(CellGcmReportData) == GPP_REPORT_STRIDE, "CellGcmReportData size = the port's report stride");
_Static_assert(offsetof(CellGcmReportData, timer) == 0, "the timer is the report entry's first field");
_Static_assert(offsetof(CellGcmReportData, value) == 8, "the value follows the 64-bit timer");
_Static_assert(GPP_LABEL_LAST <= UINT8_MAX, "cellGcmGetLabelAddress takes a uint8_t index");
_Static_assert(GPP_LABEL_USER_FIRST == 64u && GPP_LABEL_LAST == 255u && GPP_REPORTS == 2048u, "pinned ranges");

char r4b_sizeof_prof[sizeof(prof)];
char r4b_sizeof_psp_ppu[sizeof(psp_ppu)];
char r4b_sizeof_psp_lane[sizeof(psp_lane)];
char r4b_sizeof_gpu_slot[sizeof(gpu_slot)];
char r4b_sizeof_gpp_port[sizeof(gpp_port)];
char r4b_sizeof_ext_acc[sizeof(ext_acc)];
