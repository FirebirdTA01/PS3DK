#include <sys/prx_module.h>

PRX_LIBRARY_BEGIN(hello, "hello_prx")
	PRX_EXPORT_FUNC(prx_add, 0xe1f4c294)
PRX_LIBRARY_END(hello)
