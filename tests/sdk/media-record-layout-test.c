/* Fixed ABI sizes, member widths and offsets for system-library records.
 * Numeric expectations are independent of the headers under test. */
#include <stddef.h>
#ifdef __cplusplus
#define CHECK(c,m) static_assert(c,m)
#else
#define CHECK(c,m) _Static_assert(c,m)
#endif
#define SIZE(t,n) CHECK(sizeof(t)==(n), #t " size")
#define ALIGN(t,n) CHECK(__alignof__(t)==(n), #t " alignment")
#define OFFSET(t,f,n) CHECK(offsetof(t,f)==(n), #t "." #f " offset")
#define WIDTH(t,f,n) CHECK(sizeof(((t*)0)->f)==(n), #t "." #f " width")

CHECK(sizeof(void *) == EXPECTED_ABI_BYTES, "actual pointer data model");
CHECK(sizeof(long) == EXPECTED_ABI_BYTES, "actual long data model");

#if MEDIA_GROUP == 0
#include <cell/voice.h>
SIZE(CellVoiceBasePortInfo, 24);
ALIGN(CellVoiceBasePortInfo, 64);
WIDTH(CellVoiceBasePortInfo, portType, 4);
OFFSET(CellVoiceBasePortInfo, portType, 0);
WIDTH(CellVoiceBasePortInfo, state, 4);
OFFSET(CellVoiceBasePortInfo, state, 4);
WIDTH(CellVoiceBasePortInfo, numEdge, 2);
OFFSET(CellVoiceBasePortInfo, numEdge, 8);
WIDTH(CellVoiceBasePortInfo, pEdge, 4);
OFFSET(CellVoiceBasePortInfo, pEdge, 12);
WIDTH(CellVoiceBasePortInfo, numByte, 4);
OFFSET(CellVoiceBasePortInfo, numByte, 16);
WIDTH(CellVoiceBasePortInfo, frameSize, 4);
OFFSET(CellVoiceBasePortInfo, frameSize, 20);
CellVoiceBasePortInfo layout_CellVoiceBasePortInfo;
SIZE(CellVoicePortParam, 28);
ALIGN(CellVoicePortParam, 64);
WIDTH(CellVoicePortParam, portType, 4);
OFFSET(CellVoicePortParam, portType, 0);
WIDTH(CellVoicePortParam, threshold, 2);
OFFSET(CellVoicePortParam, threshold, 4);
WIDTH(CellVoicePortParam, bMute, 2);
OFFSET(CellVoicePortParam, bMute, 6);
WIDTH(CellVoicePortParam, volume, 4);
OFFSET(CellVoicePortParam, volume, 8);
WIDTH(CellVoicePortParam, voice, 4);
OFFSET(CellVoicePortParam, voice, 12);
WIDTH(CellVoicePortParam, pcmaudio, 16);
OFFSET(CellVoicePortParam, pcmaudio, 12);
WIDTH(CellVoicePortParam, device, 4);
OFFSET(CellVoicePortParam, device, 12);
CellVoicePortParam layout_CellVoicePortParam;
#endif

#if MEDIA_GROUP == 1
#include <cell/dbgfont.h>
SIZE(CellDbgFontConfigGcm, 28);
ALIGN(CellDbgFontConfigGcm, 4);
WIDTH(CellDbgFontConfigGcm, localBufAddr, 4);
OFFSET(CellDbgFontConfigGcm, localBufAddr, 0);
WIDTH(CellDbgFontConfigGcm, localBufSize, 4);
OFFSET(CellDbgFontConfigGcm, localBufSize, 4);
WIDTH(CellDbgFontConfigGcm, mainBufAddr, 4);
OFFSET(CellDbgFontConfigGcm, mainBufAddr, 8);
WIDTH(CellDbgFontConfigGcm, mainBufSize, 4);
OFFSET(CellDbgFontConfigGcm, mainBufSize, 12);
WIDTH(CellDbgFontConfigGcm, option, 4);
OFFSET(CellDbgFontConfigGcm, option, 16);
WIDTH(CellDbgFontConfigGcm, screenWidth, 4);
OFFSET(CellDbgFontConfigGcm, screenWidth, 20);
WIDTH(CellDbgFontConfigGcm, screenHeight, 4);
OFFSET(CellDbgFontConfigGcm, screenHeight, 24);
CellDbgFontConfigGcm layout_CellDbgFontConfigGcm;
SIZE(CellDbgFontConsoleConfig, 24);
ALIGN(CellDbgFontConsoleConfig, 4);
WIDTH(CellDbgFontConsoleConfig, posLeft, 4);
OFFSET(CellDbgFontConsoleConfig, posLeft, 0);
WIDTH(CellDbgFontConsoleConfig, posTop, 4);
OFFSET(CellDbgFontConsoleConfig, posTop, 4);
WIDTH(CellDbgFontConsoleConfig, cnsWidth, 4);
OFFSET(CellDbgFontConsoleConfig, cnsWidth, 8);
WIDTH(CellDbgFontConsoleConfig, cnsHeight, 4);
OFFSET(CellDbgFontConsoleConfig, cnsHeight, 12);
WIDTH(CellDbgFontConsoleConfig, scale, 4);
OFFSET(CellDbgFontConsoleConfig, scale, 16);
WIDTH(CellDbgFontConsoleConfig, color, 4);
OFFSET(CellDbgFontConsoleConfig, color, 20);
CellDbgFontConsoleConfig layout_CellDbgFontConsoleConfig;
#endif

#if MEDIA_GROUP == 2
#include <cell/sysutil_search.h>
SIZE(CellSearchTimeInfo, 24);
ALIGN(CellSearchTimeInfo, 8);
WIDTH(CellSearchTimeInfo, takenDate, 8);
OFFSET(CellSearchTimeInfo, takenDate, 0);
WIDTH(CellSearchTimeInfo, importedDate, 8);
OFFSET(CellSearchTimeInfo, importedDate, 8);
WIDTH(CellSearchTimeInfo, modifiedDate, 8);
OFFSET(CellSearchTimeInfo, modifiedDate, 16);
CellSearchTimeInfo layout_CellSearchTimeInfo;
SIZE(CellSearchMusicInfo, 1632);
ALIGN(CellSearchMusicInfo, 8);
WIDTH(CellSearchMusicInfo, duration, 8);
OFFSET(CellSearchMusicInfo, duration, 0);
WIDTH(CellSearchMusicInfo, size, 8);
OFFSET(CellSearchMusicInfo, size, 8);
WIDTH(CellSearchMusicInfo, importedDate, 8);
OFFSET(CellSearchMusicInfo, importedDate, 16);
WIDTH(CellSearchMusicInfo, lastPlayedDate, 8);
OFFSET(CellSearchMusicInfo, lastPlayedDate, 24);
WIDTH(CellSearchMusicInfo, releasedYear, 4);
OFFSET(CellSearchMusicInfo, releasedYear, 32);
WIDTH(CellSearchMusicInfo, trackNumber, 4);
OFFSET(CellSearchMusicInfo, trackNumber, 36);
WIDTH(CellSearchMusicInfo, bitrate, 4);
OFFSET(CellSearchMusicInfo, bitrate, 40);
WIDTH(CellSearchMusicInfo, samplingRate, 4);
OFFSET(CellSearchMusicInfo, samplingRate, 44);
WIDTH(CellSearchMusicInfo, quantizationBitrate, 4);
OFFSET(CellSearchMusicInfo, quantizationBitrate, 48);
WIDTH(CellSearchMusicInfo, playCount, 4);
OFFSET(CellSearchMusicInfo, playCount, 52);
WIDTH(CellSearchMusicInfo, drmEncrypted, 4);
OFFSET(CellSearchMusicInfo, drmEncrypted, 56);
WIDTH(CellSearchMusicInfo, codec, 4);
OFFSET(CellSearchMusicInfo, codec, 60);
WIDTH(CellSearchMusicInfo, status, 4);
OFFSET(CellSearchMusicInfo, status, 64);
WIDTH(CellSearchMusicInfo, diskNumber, 8);
OFFSET(CellSearchMusicInfo, diskNumber, 68);
WIDTH(CellSearchMusicInfo, title, 385);
OFFSET(CellSearchMusicInfo, title, 76);
WIDTH(CellSearchMusicInfo, reserved, 3);
OFFSET(CellSearchMusicInfo, reserved, 461);
WIDTH(CellSearchMusicInfo, albumTitle, 385);
OFFSET(CellSearchMusicInfo, albumTitle, 464);
WIDTH(CellSearchMusicInfo, reserved2, 3);
OFFSET(CellSearchMusicInfo, reserved2, 849);
WIDTH(CellSearchMusicInfo, artistName, 385);
OFFSET(CellSearchMusicInfo, artistName, 852);
WIDTH(CellSearchMusicInfo, reserved3, 3);
OFFSET(CellSearchMusicInfo, reserved3, 1237);
WIDTH(CellSearchMusicInfo, genreName, 385);
OFFSET(CellSearchMusicInfo, genreName, 1240);
WIDTH(CellSearchMusicInfo, reserved4, 3);
OFFSET(CellSearchMusicInfo, reserved4, 1625);
CellSearchMusicInfo layout_CellSearchMusicInfo;
SIZE(CellSearchPhotoInfo, 824);
ALIGN(CellSearchPhotoInfo, 8);
WIDTH(CellSearchPhotoInfo, size, 8);
OFFSET(CellSearchPhotoInfo, size, 0);
WIDTH(CellSearchPhotoInfo, importedDate, 8);
OFFSET(CellSearchPhotoInfo, importedDate, 8);
WIDTH(CellSearchPhotoInfo, takenDate, 8);
OFFSET(CellSearchPhotoInfo, takenDate, 16);
WIDTH(CellSearchPhotoInfo, width, 4);
OFFSET(CellSearchPhotoInfo, width, 24);
WIDTH(CellSearchPhotoInfo, height, 4);
OFFSET(CellSearchPhotoInfo, height, 28);
WIDTH(CellSearchPhotoInfo, orientation, 4);
OFFSET(CellSearchPhotoInfo, orientation, 32);
WIDTH(CellSearchPhotoInfo, codec, 4);
OFFSET(CellSearchPhotoInfo, codec, 36);
WIDTH(CellSearchPhotoInfo, status, 4);
OFFSET(CellSearchPhotoInfo, status, 40);
WIDTH(CellSearchPhotoInfo, title, 385);
OFFSET(CellSearchPhotoInfo, title, 44);
WIDTH(CellSearchPhotoInfo, reserved, 3);
OFFSET(CellSearchPhotoInfo, reserved, 429);
WIDTH(CellSearchPhotoInfo, albumTitle, 385);
OFFSET(CellSearchPhotoInfo, albumTitle, 432);
WIDTH(CellSearchPhotoInfo, reserved2, 3);
OFFSET(CellSearchPhotoInfo, reserved2, 817);
CellSearchPhotoInfo layout_CellSearchPhotoInfo;
SIZE(CellSearchVideoInfo, 832);
ALIGN(CellSearchVideoInfo, 8);
WIDTH(CellSearchVideoInfo, duration, 8);
OFFSET(CellSearchVideoInfo, duration, 0);
WIDTH(CellSearchVideoInfo, size, 8);
OFFSET(CellSearchVideoInfo, size, 8);
WIDTH(CellSearchVideoInfo, importedDate, 8);
OFFSET(CellSearchVideoInfo, importedDate, 16);
WIDTH(CellSearchVideoInfo, takenDate, 8);
OFFSET(CellSearchVideoInfo, takenDate, 24);
WIDTH(CellSearchVideoInfo, videoBitrate, 4);
OFFSET(CellSearchVideoInfo, videoBitrate, 32);
WIDTH(CellSearchVideoInfo, audioBitrate, 4);
OFFSET(CellSearchVideoInfo, audioBitrate, 36);
WIDTH(CellSearchVideoInfo, drmEncrypted, 4);
OFFSET(CellSearchVideoInfo, drmEncrypted, 40);
WIDTH(CellSearchVideoInfo, videoCodec, 4);
OFFSET(CellSearchVideoInfo, videoCodec, 44);
WIDTH(CellSearchVideoInfo, audioCodec, 4);
OFFSET(CellSearchVideoInfo, audioCodec, 48);
WIDTH(CellSearchVideoInfo, status, 4);
OFFSET(CellSearchVideoInfo, status, 52);
WIDTH(CellSearchVideoInfo, title, 385);
OFFSET(CellSearchVideoInfo, title, 56);
WIDTH(CellSearchVideoInfo, reserved, 3);
OFFSET(CellSearchVideoInfo, reserved, 441);
WIDTH(CellSearchVideoInfo, albumTitle, 385);
OFFSET(CellSearchVideoInfo, albumTitle, 444);
WIDTH(CellSearchVideoInfo, reserved2, 3);
OFFSET(CellSearchVideoInfo, reserved2, 829);
CellSearchVideoInfo layout_CellSearchVideoInfo;
#endif

#if MEDIA_GROUP == 3
#include <cell/sail.h>
SIZE(CellSailStartCommand, 40);
ALIGN(CellSailStartCommand, 8);
WIDTH(CellSailStartCommand, startType, 4);
OFFSET(CellSailStartCommand, startType, 0);
WIDTH(CellSailStartCommand, seekType, 4);
OFFSET(CellSailStartCommand, seekType, 4);
WIDTH(CellSailStartCommand, terminusType, 4);
OFFSET(CellSailStartCommand, terminusType, 8);
WIDTH(CellSailStartCommand, flags, 4);
OFFSET(CellSailStartCommand, flags, 12);
WIDTH(CellSailStartCommand, startArg, 4);
OFFSET(CellSailStartCommand, startArg, 16);
WIDTH(CellSailStartCommand, reserved, 4);
OFFSET(CellSailStartCommand, reserved, 20);
WIDTH(CellSailStartCommand, seekArg, 8);
OFFSET(CellSailStartCommand, seekArg, 24);
WIDTH(CellSailStartCommand, terminusArg, 8);
OFFSET(CellSailStartCommand, terminusArg, 32);
CellSailStartCommand layout_CellSailStartCommand;
/* The source callback command is a separate, unchanged record. */
SIZE(CellSailSourceStartCommand, 40);
ALIGN(CellSailSourceStartCommand, 8);
OFFSET(CellSailSourceStartCommand, startFlags, 0);
OFFSET(CellSailSourceStartCommand, startArg, 8);
OFFSET(CellSailSourceStartCommand, lengthArg, 16);
OFFSET(CellSailSourceStartCommand, optionalArg0, 24);
OFFSET(CellSailSourceStartCommand, optionalArg1, 32);
#endif
