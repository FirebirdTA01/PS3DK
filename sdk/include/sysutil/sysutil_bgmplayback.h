/*
 * PS3 Custom Toolchain - sysutil/sysutil_bgmplayback.h
 *
 * sysutil-tree alias for cell/sysutil_bgmplayback.h.  Samples that include
 * <sysutil/sysutil_bgmplayback.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_BGMPLAYBACK_H_
#define _PS3DK_SYSUTIL_BGMPLAYBACK_H_
#include <cell/sysutil_bgmplayback.h>
#endif
