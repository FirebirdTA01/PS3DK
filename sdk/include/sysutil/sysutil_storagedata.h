/*
 * PS3 Custom Toolchain - sysutil/sysutil_storagedata.h
 *
 * sysutil-tree alias for cell/sysutil_storagedata.h.  Samples that include
 * <sysutil/sysutil_storagedata.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_STORAGEDATA_H_
#define _PS3DK_SYSUTIL_STORAGEDATA_H_
#include <cell/sysutil_storagedata.h>
#endif
