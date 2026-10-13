/*
 * PS3 Custom Toolchain - sysutil/sysutil_avc2.h
 *
 * sysutil-tree alias for cell/sysutil_avc2.h.  Samples that include
 * <sysutil/sysutil_avc2.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_AVC2_H_
#define _PS3DK_SYSUTIL_AVC2_H_
#include <cell/sysutil_avc2.h>
#endif
