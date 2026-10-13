/*
 * PS3 Custom Toolchain - sysutil/sysutil_music_decode.h
 *
 * sysutil-tree alias for cell/sysutil_music_decode.h.  Samples that include
 * <sysutil/sysutil_music_decode.h> with -I .../include/sysutil resolve to the real
 * cellSysutil surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_MUSIC_DECODE_H_
#define _PS3DK_SYSUTIL_MUSIC_DECODE_H_
#include <cell/sysutil_music_decode.h>
#endif
