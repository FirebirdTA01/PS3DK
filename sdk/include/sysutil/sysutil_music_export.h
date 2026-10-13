/*
 * PS3 Custom Toolchain - sysutil/sysutil_music_export.h
 *
 * sysutil-tree alias for cell/sysutil_music_export.h.  Samples that include
 * <sysutil/sysutil_music_export.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_MUSIC_EXPORT_H_
#define _PS3DK_SYSUTIL_MUSIC_EXPORT_H_
#include <cell/sysutil_music_export.h>
#endif
