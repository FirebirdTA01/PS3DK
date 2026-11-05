/* P3.3 R4b target SIZE/ASSERT probe (compiled, never linked or run): the ILP32 sizes of the extension's structures,
 * each as the size of an array the nm -S of this object reports, and the GCM layout / constant assertions the GPU
 * binding relies on (review 22:26: in the target fixture). */
#include <stddef.h>
#include <stdint.h>

#include <cell/gcm.h>

#include "prof_internal.h"
#include "gpu_internal.h"
#include "gpu_port_gcm.h"
#include "psp_ppu.h"

_Static_assert(sizeof(void *) == 4 && sizeof(uintptr_t) == 4, "ILP32");
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
