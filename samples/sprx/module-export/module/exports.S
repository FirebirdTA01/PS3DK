#include <sys/prx_module.h>

PRX_LIBRARY_BEGIN(sample_module_export, "sample_module_export")
	PRX_EXPORT_FUNC(sample_calc_add, 0xd518c5f1)
	PRX_EXPORT_FUNC(sample_get_start_counter, 0xb803eadf)
PRX_LIBRARY_END(sample_module_export)
