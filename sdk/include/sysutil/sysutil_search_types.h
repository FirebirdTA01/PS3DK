/*
 * PS3 Custom Toolchain - sysutil/sysutil_search_types.h
 *
 * sysutil-tree alias for cell/sysutil_search_types.h.  Samples that include
 * <sysutil/sysutil_search_types.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_SEARCH_TYPES_H_
#define _PS3DK_SYSUTIL_SEARCH_TYPES_H_
#include <cell/sysutil_search_types.h>
#endif
