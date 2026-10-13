/*
 * PS3 Custom Toolchain - sysutil/sysutil_sysconf.h
 *
 * sysutil-tree alias for cell/sysutil_sysconf.h.  Samples that include
 * <sysutil/sysutil_sysconf.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_SYSCONF_H_
#define _PS3DK_SYSUTIL_SYSCONF_H_
#include <cell/sysutil_sysconf.h>
#endif
