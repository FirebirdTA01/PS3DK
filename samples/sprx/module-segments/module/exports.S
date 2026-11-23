#include <sys/prx_module.h>

/* Two named libraries; with the crt's management record that makes three
 * export records, which is what a 0x2c record size broke on hardware. */

PRX_LIBRARY_BEGIN(seg_calc, "seg_calc")
	PRX_EXPORT_FUNC(seg_calc_add, 0xbfea24b9)
	PRX_EXPORT_FUNC(seg_calc_scale, 0xfc857344)
PRX_LIBRARY_END(seg_calc)

PRX_LIBRARY_BEGIN(seg_state, "seg_state")
	PRX_EXPORT_FUNC(seg_state_data, 0xe99aac83)
	PRX_EXPORT_FUNC(seg_state_bss, 0xe5999f9b)
	PRX_EXPORT_FUNC(seg_state_bump, 0x82e12b5c)
PRX_LIBRARY_END(seg_state)
