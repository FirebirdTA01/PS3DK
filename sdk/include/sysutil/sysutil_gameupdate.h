/*
 * PS3 Custom Toolchain - sysutil/sysutil_gameupdate.h
 *
 * sysutil-tree alias for cell/sysutil_gameupdate.h.  Samples that include
 * <sysutil/sysutil_gameupdate.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_GAMEUPDATE_H_
#define _PS3DK_SYSUTIL_GAMEUPDATE_H_
#include <cell/sysutil_gameupdate.h>
#endif
