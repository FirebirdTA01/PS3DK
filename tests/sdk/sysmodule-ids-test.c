/*
 * <cell/sysmodule.h> module ids are an ABI: cellSysmoduleLoadModule() takes
 * the number, and a wrong one loads a different firmware module without any
 * error.  NP Commerce2 was 0x003e (the game sysutil module's id) and NP SNS
 * 0xf043; CELL_SYSMODULE_SYSUTIL_GAME and 25 other ids were missing.
 */
#include <cell/sysmodule.h>

#ifdef __cplusplus
#define PIN(name, value) static_assert(CELL_SYSMODULE_##name == (value), #name)
#else
#define PIN(name, value) _Static_assert(CELL_SYSMODULE_##name == (value), #name)
#endif

PIN(SYSUTIL_GAME, 0x003e);
PIN(SYSUTIL_NP_COMMERCE2, 0x0044);
PIN(COMMERCE2, 0x0044);
PIN(SYSUTIL_NP_SNS, 0x0059);
PIN(SNS, 0x0059);

PIN(VDEC_AL, 0x002b);
PIN(ADEC_AL, 0x002c);
PIN(USBPSPCM, 0x0030);
PIN(AVCONF_EXT, 0x0031);
PIN(BGDL, 0x003f);
PIN(FREETYPE_TT, 0x0040);
PIN(SYSUTIL_VIDEO_UPLOAD, 0x0041);
PIN(SYSUTIL_SYSCONF_EXT, 0x0042);
PIN(SYSUTIL_LICENSEAREA, 0x0049);
PIN(SYSUTIL_MUSIC2, 0x004a);
PIN(SYSUTIL_NP_UTIL, 0x0056);
PIN(GEM, 0x005a);
PIN(SYSUTIL_CROSS_CONTROLLER, 0x005c);
PIN(ADEC_M2BC, 0xf01b);
PIN(ADEC_M4AAC, 0xf01d);
PIN(ADEC_MP3, 0xf01e);
PIN(IMEJP, 0xf023);
PIN(SYSUTIL_MUSIC, 0xf028);
PIN(PHOTO_EXPORT, 0xf029);
PIN(PRINT, 0xf02a);
PIN(PHOTO_IMPORT, 0xf02b);
PIN(MUSIC_EXPORT, 0xf02c);
PIN(PHOTO_DECODE, 0xf02e);
PIN(SYSUTIL_SEARCH, 0xf02f);
PIN(SYSUTIL_AVCHAT2, 0xf030);

/* neighbours of the corrected ids keep their values */
PIN(VDEC_DIVX, 0x003c);
PIN(JPGENC, 0x003d);
PIN(FIBER, 0x0043);
PIN(SYSUTIL_NP_TUS, 0x0045);
PIN(SYSUTIL_NP_TROPHY, 0xf035);
PIN(SYSUTIL_GAME_EXEC, 0x0037);

int sysmodule_ids_probe;
