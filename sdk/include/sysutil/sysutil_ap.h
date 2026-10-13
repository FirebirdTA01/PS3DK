/*
 * PS3 Custom Toolchain - sysutil/sysutil_ap.h
 *
 * sysutil-tree alias for cell/sysutil_ap.h.  Samples that include
 * <sysutil/sysutil_ap.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_AP_H_
#define _PS3DK_SYSUTIL_AP_H_
#include <cell/sysutil_ap.h>
#endif
