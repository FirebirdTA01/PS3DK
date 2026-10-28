/* Compile-time ABI guard for <cell/sail.h>, <cell/sail/muxer.h>, <cell/sail/recorder.h>:
 * the system's multimedia services exchange these records with user space,
 * so every size, alignment and offset is part of the system interface.
 * Both data models use the system's 32-bit layout; pointer members and callback
 * slots are 32-bit effective addresses (ATTRIBUTE_PRXPTR). */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <cell/sail.h>
#include <cell/sail/recorder.h>
#include <cell/sail/muxer.h>

#ifdef __cplusplus
#define CHECK(c, m) static_assert(c, m)
#else
#define CHECK(c, m) _Static_assert(c, m)
#endif
#define SIZE(t, n) CHECK(sizeof(t) == (n), #t " size")
#define ALIGN(t, n) CHECK(__alignof__(t) == (n), #t " alignment")
#define OFFSET(t, f, n) CHECK(offsetof(t, f) == (n), #t "." #f " offset")
#define WIDTH(t, f, n) CHECK(sizeof(((t *)0)->f) == (n), #t "." #f " width")

/* The runner passes the data model it asked for; prove the compiler used it. */
CHECK(sizeof(void *) == EXPECTED_ABI_BYTES, "actual pointer data model");
CHECK(sizeof(long) == EXPECTED_ABI_BYTES, "actual long data model");

/* Mutant controls for test runner verification */
#if defined(MUTANT_SAIL_PLAYER_SIZE)
CHECK(sizeof(CellSailPlayer) == 2048, "mutant: old 2048-byte player size");
#elif defined(MUTANT_SAIL_RECORDER_SIZE)
CHECK(sizeof(CellSailRecorder) == 1024, "mutant: old 1024-byte recorder size");
#elif defined(MUTANT_SAIL_COMPOSER_SIZE)
CHECK(sizeof(CellSailComposer) == 256, "mutant: old 256-byte composer size");
#elif defined(MUTANT_SAIL_VIDEO_FORMAT_SIZE)
CHECK(sizeof(CellSailVideoFormat) == 40, "mutant: old 40-byte video format size");
#elif defined(MUTANT_SAIL_COMPOSER_VIDEO_AU_PTS)
CHECK(sizeof(((CellSailComposerVideoAuInfo *)0)->pts) == 8, "mutant: scalar pts instead of array");
#elif defined(MUTANT_SAIL_AVI_STREAM_HEADER_SIZE)
CHECK(sizeof(CellSailAviStreamHeader) == 60, "mutant: old 60-byte avi stream header");
#endif

/* CellSailAviStreamHeader */
SIZE(CellSailAviStreamHeader, 56);
ALIGN(CellSailAviStreamHeader, 4);
OFFSET(CellSailAviStreamHeader, fccType, 0);
WIDTH(CellSailAviStreamHeader, fccType, 4);
OFFSET(CellSailAviStreamHeader, fccHandler, 4);
WIDTH(CellSailAviStreamHeader, fccHandler, 4);
OFFSET(CellSailAviStreamHeader, flags, 8);
WIDTH(CellSailAviStreamHeader, flags, 4);
OFFSET(CellSailAviStreamHeader, priority, 12);
WIDTH(CellSailAviStreamHeader, priority, 2);
OFFSET(CellSailAviStreamHeader, language, 14);
WIDTH(CellSailAviStreamHeader, language, 2);
OFFSET(CellSailAviStreamHeader, initialFrames, 16);
WIDTH(CellSailAviStreamHeader, initialFrames, 4);
OFFSET(CellSailAviStreamHeader, scale, 20);
WIDTH(CellSailAviStreamHeader, scale, 4);
OFFSET(CellSailAviStreamHeader, rate, 24);
WIDTH(CellSailAviStreamHeader, rate, 4);
OFFSET(CellSailAviStreamHeader, start, 28);
WIDTH(CellSailAviStreamHeader, start, 4);
OFFSET(CellSailAviStreamHeader, length, 32);
WIDTH(CellSailAviStreamHeader, length, 4);
OFFSET(CellSailAviStreamHeader, suggestedBufferSize, 36);
WIDTH(CellSailAviStreamHeader, suggestedBufferSize, 4);
OFFSET(CellSailAviStreamHeader, quality, 40);
WIDTH(CellSailAviStreamHeader, quality, 4);
OFFSET(CellSailAviStreamHeader, sampleSize, 44);
WIDTH(CellSailAviStreamHeader, sampleSize, 4);
OFFSET(CellSailAviStreamHeader, frame, 48);
WIDTH(CellSailAviStreamHeader, frame, 8);
OFFSET(CellSailAviStreamHeader, left, 48);
WIDTH(CellSailAviStreamHeader, left, 2);

/* CellSailMp4DateTime */
SIZE(CellSailMp4DateTime, 16);
ALIGN(CellSailMp4DateTime, 2);
OFFSET(CellSailMp4DateTime, second, 0);
WIDTH(CellSailMp4DateTime, second, 2);
OFFSET(CellSailMp4DateTime, s, 0);
WIDTH(CellSailMp4DateTime, s, 2);
OFFSET(CellSailMp4DateTime, minute, 2);
WIDTH(CellSailMp4DateTime, minute, 2);
OFFSET(CellSailMp4DateTime, m, 2);
WIDTH(CellSailMp4DateTime, m, 2);
OFFSET(CellSailMp4DateTime, hour, 4);
WIDTH(CellSailMp4DateTime, hour, 2);
OFFSET(CellSailMp4DateTime, h, 4);
WIDTH(CellSailMp4DateTime, h, 2);
OFFSET(CellSailMp4DateTime, day, 6);
WIDTH(CellSailMp4DateTime, day, 2);
OFFSET(CellSailMp4DateTime, d, 6);
WIDTH(CellSailMp4DateTime, d, 2);
OFFSET(CellSailMp4DateTime, month, 8);
WIDTH(CellSailMp4DateTime, month, 2);
OFFSET(CellSailMp4DateTime, mo, 8);
WIDTH(CellSailMp4DateTime, mo, 2);
OFFSET(CellSailMp4DateTime, year, 10);
WIDTH(CellSailMp4DateTime, year, 2);
OFFSET(CellSailMp4DateTime, y, 10);
WIDTH(CellSailMp4DateTime, y, 2);
OFFSET(CellSailMp4DateTime, reserved0, 12);
WIDTH(CellSailMp4DateTime, reserved0, 2);
OFFSET(CellSailMp4DateTime, res0, 12);
WIDTH(CellSailMp4DateTime, res0, 2);
OFFSET(CellSailMp4DateTime, reserved1, 14);
WIDTH(CellSailMp4DateTime, reserved1, 2);
OFFSET(CellSailMp4DateTime, res1, 14);
WIDTH(CellSailMp4DateTime, res1, 2);

/* CellSailMp4TrackInfo */
SIZE(CellSailMp4TrackInfo, 64);
ALIGN(CellSailMp4TrackInfo, 8);
OFFSET(CellSailMp4TrackInfo, isTrackEnable, 0);
WIDTH(CellSailMp4TrackInfo, isTrackEnable, 1);
OFFSET(CellSailMp4TrackInfo, reserved0, 1);
WIDTH(CellSailMp4TrackInfo, reserved0, 3);
OFFSET(CellSailMp4TrackInfo, trackId, 4);
WIDTH(CellSailMp4TrackInfo, trackId, 4);
OFFSET(CellSailMp4TrackInfo, trackDuration, 8);
WIDTH(CellSailMp4TrackInfo, trackDuration, 8);
OFFSET(CellSailMp4TrackInfo, layer, 16);
WIDTH(CellSailMp4TrackInfo, layer, 2);
OFFSET(CellSailMp4TrackInfo, alternateGroup, 18);
WIDTH(CellSailMp4TrackInfo, alternateGroup, 2);
OFFSET(CellSailMp4TrackInfo, reserved1, 20);
WIDTH(CellSailMp4TrackInfo, reserved1, 4);
OFFSET(CellSailMp4TrackInfo, trackWidth, 24);
WIDTH(CellSailMp4TrackInfo, trackWidth, 4);
OFFSET(CellSailMp4TrackInfo, trackHeight, 28);
WIDTH(CellSailMp4TrackInfo, trackHeight, 4);
OFFSET(CellSailMp4TrackInfo, language, 32);
WIDTH(CellSailMp4TrackInfo, language, 2);
OFFSET(CellSailMp4TrackInfo, reserved2, 34);
WIDTH(CellSailMp4TrackInfo, reserved2, 2);
OFFSET(CellSailMp4TrackInfo, mediaType, 36);
WIDTH(CellSailMp4TrackInfo, mediaType, 4);
OFFSET(CellSailMp4TrackInfo, reserved3, 40);
WIDTH(CellSailMp4TrackInfo, reserved3, 24);

/* CellSailPlayer */
SIZE(CellSailPlayer, 1024);
ALIGN(CellSailPlayer, 8);
OFFSET(CellSailPlayer, internalData, 0);
WIDTH(CellSailPlayer, internalData, 1024);

/* CellSailVideoFormat */
SIZE(CellSailVideoFormat, 32);
ALIGN(CellSailVideoFormat, 8);
OFFSET(CellSailVideoFormat, coding, 0);
WIDTH(CellSailVideoFormat, coding, 1);
OFFSET(CellSailVideoFormat, scan, 1);
WIDTH(CellSailVideoFormat, scan, 1);
OFFSET(CellSailVideoFormat, bitsPerColor, 2);
WIDTH(CellSailVideoFormat, bitsPerColor, 1);
OFFSET(CellSailVideoFormat, frameRate, 3);
WIDTH(CellSailVideoFormat, frameRate, 1);
OFFSET(CellSailVideoFormat, width, 4);
WIDTH(CellSailVideoFormat, width, 2);
OFFSET(CellSailVideoFormat, height, 6);
WIDTH(CellSailVideoFormat, height, 2);
OFFSET(CellSailVideoFormat, pitch, 8);
WIDTH(CellSailVideoFormat, pitch, 4);
OFFSET(CellSailVideoFormat, alpha, 12);
WIDTH(CellSailVideoFormat, alpha, 4);
OFFSET(CellSailVideoFormat, colorMatrix, 16);
WIDTH(CellSailVideoFormat, colorMatrix, 1);
OFFSET(CellSailVideoFormat, aspectRatio, 17);
WIDTH(CellSailVideoFormat, aspectRatio, 1);
OFFSET(CellSailVideoFormat, colorRange, 18);
WIDTH(CellSailVideoFormat, colorRange, 1);
OFFSET(CellSailVideoFormat, reserved1, 19);
WIDTH(CellSailVideoFormat, reserved1, 1);
OFFSET(CellSailVideoFormat, reserved2, 20);
WIDTH(CellSailVideoFormat, reserved2, 4);
OFFSET(CellSailVideoFormat, reserved3, 24);
WIDTH(CellSailVideoFormat, reserved3, 8);

/* CellSailRecorder */
SIZE(CellSailRecorder, 8192);
ALIGN(CellSailRecorder, 128);
OFFSET(CellSailRecorder, internalData, 0);
WIDTH(CellSailRecorder, internalData, 8192);

/* CellSailComposer */
SIZE(CellSailComposer, 320);
ALIGN(CellSailComposer, 8);
OFFSET(CellSailComposer, internalData, 0);
WIDTH(CellSailComposer, internalData, 320);

/* CellSailAuInfo */
SIZE(CellSailAuInfo, 40);
ALIGN(CellSailAuInfo, 8);
OFFSET(CellSailAuInfo, pAu, 0);
WIDTH(CellSailAuInfo, pAu, 4);
OFFSET(CellSailAuInfo, size, 4);
WIDTH(CellSailAuInfo, size, 4);
OFFSET(CellSailAuInfo, status, 8);
WIDTH(CellSailAuInfo, status, 4);
OFFSET(CellSailAuInfo, sessionId, 12);
WIDTH(CellSailAuInfo, sessionId, 4);
OFFSET(CellSailAuInfo, pts, 16);
WIDTH(CellSailAuInfo, pts, 8);
OFFSET(CellSailAuInfo, dts, 24);
WIDTH(CellSailAuInfo, dts, 8);
OFFSET(CellSailAuInfo, reserved, 32);
WIDTH(CellSailAuInfo, reserved, 8);

/* CellSailSourceBufferItem */
SIZE(CellSailSourceBufferItem, 16);
ALIGN(CellSailSourceBufferItem, 4);
OFFSET(CellSailSourceBufferItem, pBuf, 0);
WIDTH(CellSailSourceBufferItem, pBuf, 4);
OFFSET(CellSailSourceBufferItem, size, 4);
WIDTH(CellSailSourceBufferItem, size, 4);
OFFSET(CellSailSourceBufferItem, sessionId, 8);
WIDTH(CellSailSourceBufferItem, sessionId, 4);
OFFSET(CellSailSourceBufferItem, reserved, 12);
WIDTH(CellSailSourceBufferItem, reserved, 4);

/* CellSailBsVideoParameter */
SIZE(CellSailBsVideoParameter, 80);
ALIGN(CellSailBsVideoParameter, 8);
OFFSET(CellSailBsVideoParameter, thisSize, 0);
WIDTH(CellSailBsVideoParameter, thisSize, 4);
OFFSET(CellSailBsVideoParameter, codecType, 4);
WIDTH(CellSailBsVideoParameter, codecType, 4);
OFFSET(CellSailBsVideoParameter, flags, 8);
WIDTH(CellSailBsVideoParameter, flags, 4);
OFFSET(CellSailBsVideoParameter, majorMode, 12);
WIDTH(CellSailBsVideoParameter, majorMode, 4);
OFFSET(CellSailBsVideoParameter, minorMode, 16);
WIDTH(CellSailBsVideoParameter, minorMode, 4);
OFFSET(CellSailBsVideoParameter, maxBitrate, 20);
WIDTH(CellSailBsVideoParameter, maxBitrate, 4);
OFFSET(CellSailBsVideoParameter, aveBitrate, 24);
WIDTH(CellSailBsVideoParameter, aveBitrate, 4);
OFFSET(CellSailBsVideoParameter, sampleScale, 28);
WIDTH(CellSailBsVideoParameter, sampleScale, 4);
OFFSET(CellSailBsVideoParameter, gopDuration, 32);
WIDTH(CellSailBsVideoParameter, gopDuration, 4);
OFFSET(CellSailBsVideoParameter, reserved, 36);
WIDTH(CellSailBsVideoParameter, reserved, 12);
OFFSET(CellSailBsVideoParameter, format, 48);
WIDTH(CellSailBsVideoParameter, format, 32);

/* CellSailBsAudioParameter */
SIZE(CellSailBsAudioParameter, 72);
ALIGN(CellSailBsAudioParameter, 8);
OFFSET(CellSailBsAudioParameter, thisSize, 0);
WIDTH(CellSailBsAudioParameter, thisSize, 4);
OFFSET(CellSailBsAudioParameter, codecType, 4);
WIDTH(CellSailBsAudioParameter, codecType, 4);
OFFSET(CellSailBsAudioParameter, flags, 8);
WIDTH(CellSailBsAudioParameter, flags, 4);
OFFSET(CellSailBsAudioParameter, reserved0, 12);
WIDTH(CellSailBsAudioParameter, reserved0, 4);
OFFSET(CellSailBsAudioParameter, reserved1, 16);
WIDTH(CellSailBsAudioParameter, reserved1, 4);
OFFSET(CellSailBsAudioParameter, maxBitrate, 20);
WIDTH(CellSailBsAudioParameter, maxBitrate, 4);
OFFSET(CellSailBsAudioParameter, aveBitrate, 24);
WIDTH(CellSailBsAudioParameter, aveBitrate, 4);
OFFSET(CellSailBsAudioParameter, sampleScale, 28);
WIDTH(CellSailBsAudioParameter, sampleScale, 4);
OFFSET(CellSailBsAudioParameter, reserved2, 32);
WIDTH(CellSailBsAudioParameter, reserved2, 16);
OFFSET(CellSailBsAudioParameter, format, 48);
WIDTH(CellSailBsAudioParameter, format, 24);

/* CellSailComposerVideoAuInfo */
SIZE(CellSailComposerVideoAuInfo, 56);
ALIGN(CellSailComposerVideoAuInfo, 8);
OFFSET(CellSailComposerVideoAuInfo, pAu, 0);
WIDTH(CellSailComposerVideoAuInfo, pAu, 4);
OFFSET(CellSailComposerVideoAuInfo, auSize, 4);
WIDTH(CellSailComposerVideoAuInfo, auSize, 4);
OFFSET(CellSailComposerVideoAuInfo, pts, 8);
WIDTH(CellSailComposerVideoAuInfo, pts, 24);
OFFSET(CellSailComposerVideoAuInfo, dts, 32);
WIDTH(CellSailComposerVideoAuInfo, dts, 8);
OFFSET(CellSailComposerVideoAuInfo, reserved, 40);
WIDTH(CellSailComposerVideoAuInfo, reserved, 8);
OFFSET(CellSailComposerVideoAuInfo, sessionId, 48);
WIDTH(CellSailComposerVideoAuInfo, sessionId, 4);
OFFSET(CellSailComposerVideoAuInfo, specificInfo, 52);
WIDTH(CellSailComposerVideoAuInfo, specificInfo, 4);

/* CellSailComposerAudioAuInfo */
SIZE(CellSailComposerAudioAuInfo, 32);
ALIGN(CellSailComposerAudioAuInfo, 8);
OFFSET(CellSailComposerAudioAuInfo, pAu, 0);
WIDTH(CellSailComposerAudioAuInfo, pAu, 4);
OFFSET(CellSailComposerAudioAuInfo, auSize, 4);
WIDTH(CellSailComposerAudioAuInfo, auSize, 4);
OFFSET(CellSailComposerAudioAuInfo, pts, 8);
WIDTH(CellSailComposerAudioAuInfo, pts, 8);
OFFSET(CellSailComposerAudioAuInfo, reserved, 16);
WIDTH(CellSailComposerAudioAuInfo, reserved, 8);
OFFSET(CellSailComposerAudioAuInfo, sessionId, 24);
WIDTH(CellSailComposerAudioAuInfo, sessionId, 4);
OFFSET(CellSailComposerAudioAuInfo, specificInfo, 28);
WIDTH(CellSailComposerAudioAuInfo, specificInfo, 4);

/* CellSailComposerUserAuInfo */
SIZE(CellSailComposerUserAuInfo, 40);
ALIGN(CellSailComposerUserAuInfo, 8);
OFFSET(CellSailComposerUserAuInfo, pAu, 0);
WIDTH(CellSailComposerUserAuInfo, pAu, 4);
OFFSET(CellSailComposerUserAuInfo, auSize, 4);
WIDTH(CellSailComposerUserAuInfo, auSize, 4);
OFFSET(CellSailComposerUserAuInfo, pts, 8);
WIDTH(CellSailComposerUserAuInfo, pts, 8);
OFFSET(CellSailComposerUserAuInfo, dts, 16);
WIDTH(CellSailComposerUserAuInfo, dts, 8);
OFFSET(CellSailComposerUserAuInfo, reserved, 24);
WIDTH(CellSailComposerUserAuInfo, reserved, 8);
OFFSET(CellSailComposerUserAuInfo, sessionId, 32);
WIDTH(CellSailComposerUserAuInfo, sessionId, 4);
OFFSET(CellSailComposerUserAuInfo, specificInfo, 36);
WIDTH(CellSailComposerUserAuInfo, specificInfo, 4);

/* CellSailComposerEsAudioParameter */
SIZE(CellSailComposerEsAudioParameter, 72);
ALIGN(CellSailComposerEsAudioParameter, 8);
OFFSET(CellSailComposerEsAudioParameter, codecType, 0);
WIDTH(CellSailComposerEsAudioParameter, codecType, 4);
OFFSET(CellSailComposerEsAudioParameter, flags, 4);
WIDTH(CellSailComposerEsAudioParameter, flags, 4);
OFFSET(CellSailComposerEsAudioParameter, reserved0, 8);
WIDTH(CellSailComposerEsAudioParameter, reserved0, 4);
OFFSET(CellSailComposerEsAudioParameter, reserved1, 12);
WIDTH(CellSailComposerEsAudioParameter, reserved1, 4);
OFFSET(CellSailComposerEsAudioParameter, maxBitrate, 16);
WIDTH(CellSailComposerEsAudioParameter, maxBitrate, 4);
OFFSET(CellSailComposerEsAudioParameter, aveBitrate, 20);
WIDTH(CellSailComposerEsAudioParameter, aveBitrate, 4);
OFFSET(CellSailComposerEsAudioParameter, sampleScale, 24);
WIDTH(CellSailComposerEsAudioParameter, sampleScale, 4);
OFFSET(CellSailComposerEsAudioParameter, reserved2, 28);
WIDTH(CellSailComposerEsAudioParameter, reserved2, 12);
OFFSET(CellSailComposerEsAudioParameter, format, 40);
WIDTH(CellSailComposerEsAudioParameter, format, 24);
OFFSET(CellSailComposerEsAudioParameter, pSpecificInfo, 64);
WIDTH(CellSailComposerEsAudioParameter, pSpecificInfo, 4);

/* CellSailComposerEsVideoParameter */
SIZE(CellSailComposerEsVideoParameter, 80);
ALIGN(CellSailComposerEsVideoParameter, 8);
OFFSET(CellSailComposerEsVideoParameter, codecType, 0);
WIDTH(CellSailComposerEsVideoParameter, codecType, 4);
OFFSET(CellSailComposerEsVideoParameter, flags, 4);
WIDTH(CellSailComposerEsVideoParameter, flags, 4);
OFFSET(CellSailComposerEsVideoParameter, majorMode, 8);
WIDTH(CellSailComposerEsVideoParameter, majorMode, 4);
OFFSET(CellSailComposerEsVideoParameter, minorMode, 12);
WIDTH(CellSailComposerEsVideoParameter, minorMode, 4);
OFFSET(CellSailComposerEsVideoParameter, maxBitrate, 16);
WIDTH(CellSailComposerEsVideoParameter, maxBitrate, 4);
OFFSET(CellSailComposerEsVideoParameter, aveBitrate, 20);
WIDTH(CellSailComposerEsVideoParameter, aveBitrate, 4);
OFFSET(CellSailComposerEsVideoParameter, sampleScale, 24);
WIDTH(CellSailComposerEsVideoParameter, sampleScale, 4);
OFFSET(CellSailComposerEsVideoParameter, gopDuration, 28);
WIDTH(CellSailComposerEsVideoParameter, gopDuration, 4);
OFFSET(CellSailComposerEsVideoParameter, reserved, 32);
WIDTH(CellSailComposerEsVideoParameter, reserved, 8);
OFFSET(CellSailComposerEsVideoParameter, format, 40);
WIDTH(CellSailComposerEsVideoParameter, format, 32);
OFFSET(CellSailComposerEsVideoParameter, pSpecificInfo, 72);
WIDTH(CellSailComposerEsVideoParameter, pSpecificInfo, 4);

/* CellSailFeederFrameInfo */
SIZE(CellSailFeederFrameInfo, 48);
ALIGN(CellSailFeederFrameInfo, 8);
OFFSET(CellSailFeederFrameInfo, pFrame, 0);
WIDTH(CellSailFeederFrameInfo, pFrame, 4);
OFFSET(CellSailFeederFrameInfo, size, 4);
WIDTH(CellSailFeederFrameInfo, size, 4);
OFFSET(CellSailFeederFrameInfo, sampleNum, 8);
WIDTH(CellSailFeederFrameInfo, sampleNum, 4);
OFFSET(CellSailFeederFrameInfo, reserved0, 12);
WIDTH(CellSailFeederFrameInfo, reserved0, 4);
OFFSET(CellSailFeederFrameInfo, timeStamp, 16);
WIDTH(CellSailFeederFrameInfo, timeStamp, 8);
OFFSET(CellSailFeederFrameInfo, reserved1, 24);
WIDTH(CellSailFeederFrameInfo, reserved1, 8);
OFFSET(CellSailFeederFrameInfo, commandNum, 32);
WIDTH(CellSailFeederFrameInfo, commandNum, 4);
OFFSET(CellSailFeederFrameInfo, pCommands, 36);
WIDTH(CellSailFeederFrameInfo, pCommands, 4);
OFFSET(CellSailFeederFrameInfo, reserved2, 40);
WIDTH(CellSailFeederFrameInfo, reserved2, 4);
OFFSET(CellSailFeederFrameInfo, reserved3, 44);
WIDTH(CellSailFeederFrameInfo, reserved3, 4);

/* CellSailAudioFrameInfo */
SIZE(CellSailAudioFrameInfo, 24);
ALIGN(CellSailAudioFrameInfo, 8);
OFFSET(CellSailAudioFrameInfo, pPcm, 0);
WIDTH(CellSailAudioFrameInfo, pPcm, 4);
OFFSET(CellSailAudioFrameInfo, status, 4);
WIDTH(CellSailAudioFrameInfo, status, 4);
OFFSET(CellSailAudioFrameInfo, pts, 8);
WIDTH(CellSailAudioFrameInfo, pts, 8);
OFFSET(CellSailAudioFrameInfo, reserved, 16);
WIDTH(CellSailAudioFrameInfo, reserved, 8);

/* CellSailVideoFrameInfo */
SIZE(CellSailVideoFrameInfo, 32);
ALIGN(CellSailVideoFrameInfo, 8);
OFFSET(CellSailVideoFrameInfo, pPic, 0);
WIDTH(CellSailVideoFrameInfo, pPic, 4);
OFFSET(CellSailVideoFrameInfo, status, 4);
WIDTH(CellSailVideoFrameInfo, status, 4);
OFFSET(CellSailVideoFrameInfo, pts, 8);
WIDTH(CellSailVideoFrameInfo, pts, 8);
OFFSET(CellSailVideoFrameInfo, reserved, 16);
WIDTH(CellSailVideoFrameInfo, reserved, 8);
OFFSET(CellSailVideoFrameInfo, interval, 24);
WIDTH(CellSailVideoFrameInfo, interval, 2);
OFFSET(CellSailVideoFrameInfo, structure, 26);
WIDTH(CellSailVideoFrameInfo, structure, 1);
OFFSET(CellSailVideoFrameInfo, repeatNum, 27);
WIDTH(CellSailVideoFrameInfo, repeatNum, 1);
OFFSET(CellSailVideoFrameInfo, reserved2, 28);
WIDTH(CellSailVideoFrameInfo, reserved2, 4);

/* CellSailSoundFrameInfo */
SIZE(CellSailSoundFrameInfo, 24);
ALIGN(CellSailSoundFrameInfo, 8);
OFFSET(CellSailSoundFrameInfo, pBuffer, 0);
WIDTH(CellSailSoundFrameInfo, pBuffer, 4);
OFFSET(CellSailSoundFrameInfo, sessionId, 4);
WIDTH(CellSailSoundFrameInfo, sessionId, 4);
OFFSET(CellSailSoundFrameInfo, tag, 8);
WIDTH(CellSailSoundFrameInfo, tag, 4);
OFFSET(CellSailSoundFrameInfo, status, 12);
WIDTH(CellSailSoundFrameInfo, status, 4);
OFFSET(CellSailSoundFrameInfo, pts, 16);
WIDTH(CellSailSoundFrameInfo, pts, 8);

/* CellSailGraphicsFrameInfo */
SIZE(CellSailGraphicsFrameInfo, 24);
ALIGN(CellSailGraphicsFrameInfo, 8);
OFFSET(CellSailGraphicsFrameInfo, pBuffer, 0);
WIDTH(CellSailGraphicsFrameInfo, pBuffer, 4);
OFFSET(CellSailGraphicsFrameInfo, sessionId, 4);
WIDTH(CellSailGraphicsFrameInfo, sessionId, 4);
OFFSET(CellSailGraphicsFrameInfo, tag, 8);
WIDTH(CellSailGraphicsFrameInfo, tag, 4);
OFFSET(CellSailGraphicsFrameInfo, status, 12);
WIDTH(CellSailGraphicsFrameInfo, status, 4);
OFFSET(CellSailGraphicsFrameInfo, pts, 16);
WIDTH(CellSailGraphicsFrameInfo, pts, 8);

/* CellSailSoundAdapterFuncs */
SIZE(CellSailSoundAdapterFuncs, 12);
ALIGN(CellSailSoundAdapterFuncs, 4);
OFFSET(CellSailSoundAdapterFuncs, pMakeup, 0);
WIDTH(CellSailSoundAdapterFuncs, pMakeup, 4);
OFFSET(CellSailSoundAdapterFuncs, pCleanup, 4);
WIDTH(CellSailSoundAdapterFuncs, pCleanup, 4);
OFFSET(CellSailSoundAdapterFuncs, pFormatChanged, 8);
WIDTH(CellSailSoundAdapterFuncs, pFormatChanged, 4);

/* CellSailGraphicsAdapterFuncs */
SIZE(CellSailGraphicsAdapterFuncs, 20);
ALIGN(CellSailGraphicsAdapterFuncs, 4);
OFFSET(CellSailGraphicsAdapterFuncs, pMakeup, 0);
WIDTH(CellSailGraphicsAdapterFuncs, pMakeup, 4);
OFFSET(CellSailGraphicsAdapterFuncs, pCleanup, 4);
WIDTH(CellSailGraphicsAdapterFuncs, pCleanup, 4);
OFFSET(CellSailGraphicsAdapterFuncs, pFormatChanged, 8);
WIDTH(CellSailGraphicsAdapterFuncs, pFormatChanged, 4);
OFFSET(CellSailGraphicsAdapterFuncs, pAlloc, 12);
WIDTH(CellSailGraphicsAdapterFuncs, pAlloc, 4);
OFFSET(CellSailGraphicsAdapterFuncs, pFree, 16);
WIDTH(CellSailGraphicsAdapterFuncs, pFree, 4);

/* CellSailRendererAudioFuncs */
SIZE(CellSailRendererAudioFuncs, 36);
ALIGN(CellSailRendererAudioFuncs, 4);
OFFSET(CellSailRendererAudioFuncs, pMakeup, 0);
WIDTH(CellSailRendererAudioFuncs, pMakeup, 4);
OFFSET(CellSailRendererAudioFuncs, pCleanup, 4);
WIDTH(CellSailRendererAudioFuncs, pCleanup, 4);
OFFSET(CellSailRendererAudioFuncs, pOpen, 8);
WIDTH(CellSailRendererAudioFuncs, pOpen, 4);
OFFSET(CellSailRendererAudioFuncs, pClose, 12);
WIDTH(CellSailRendererAudioFuncs, pClose, 4);
OFFSET(CellSailRendererAudioFuncs, pStart, 16);
WIDTH(CellSailRendererAudioFuncs, pStart, 4);
OFFSET(CellSailRendererAudioFuncs, pStop, 20);
WIDTH(CellSailRendererAudioFuncs, pStop, 4);
OFFSET(CellSailRendererAudioFuncs, pCancel, 24);
WIDTH(CellSailRendererAudioFuncs, pCancel, 4);
OFFSET(CellSailRendererAudioFuncs, pCheckout, 28);
WIDTH(CellSailRendererAudioFuncs, pCheckout, 4);
OFFSET(CellSailRendererAudioFuncs, pCheckin, 32);
WIDTH(CellSailRendererAudioFuncs, pCheckin, 4);

/* CellSailRendererVideoFuncs */
SIZE(CellSailRendererVideoFuncs, 36);
ALIGN(CellSailRendererVideoFuncs, 4);
OFFSET(CellSailRendererVideoFuncs, pMakeup, 0);
WIDTH(CellSailRendererVideoFuncs, pMakeup, 4);
OFFSET(CellSailRendererVideoFuncs, pCleanup, 4);
WIDTH(CellSailRendererVideoFuncs, pCleanup, 4);
OFFSET(CellSailRendererVideoFuncs, pOpen, 8);
WIDTH(CellSailRendererVideoFuncs, pOpen, 4);
OFFSET(CellSailRendererVideoFuncs, pClose, 12);
WIDTH(CellSailRendererVideoFuncs, pClose, 4);
OFFSET(CellSailRendererVideoFuncs, pStart, 16);
WIDTH(CellSailRendererVideoFuncs, pStart, 4);
OFFSET(CellSailRendererVideoFuncs, pStop, 20);
WIDTH(CellSailRendererVideoFuncs, pStop, 4);
OFFSET(CellSailRendererVideoFuncs, pCancel, 24);
WIDTH(CellSailRendererVideoFuncs, pCancel, 4);
OFFSET(CellSailRendererVideoFuncs, pCheckout, 28);
WIDTH(CellSailRendererVideoFuncs, pCheckout, 4);
OFFSET(CellSailRendererVideoFuncs, pCheckin, 32);
WIDTH(CellSailRendererVideoFuncs, pCheckin, 4);

/* Real objects, so every record is laid out by the compiler, not only measured. */
CellSailPlayer sail_layout_player;
CellSailRecorder sail_layout_recorder;
CellSailComposer sail_layout_composer;
CellSailAudioFrameInfo sail_layout_audio_frame;
CellSailVideoFrameInfo sail_layout_video_frame;
CellSailSoundFrameInfo sail_layout_sound_frame;
CellSailGraphicsFrameInfo sail_layout_graphics_frame;
CellSailBsVideoParameter sail_layout_video_param;
CellSailBsAudioParameter sail_layout_audio_param;
CellSailComposerVideoAuInfo sail_layout_composer_video;
CellSailSoundAdapterFuncs sail_layout_sound_funcs;
CellSailRendererAudioFuncs sail_layout_renderer_audio_funcs;

/* Pointer type semantics check: ensure pointer fields reject integer assignment
 * and have the exact expected pointer types in both ILP32 and LP64. */
static int dummy_cb_makeup(void *arg) { (void)arg; return 0; }
static int dummy_cb_cleanup(void *arg) { (void)arg; return 0; }
static int dummy_cb_format_changed(void *arg, CellSailAudioFormat *fmt, uint32_t val) {
    (void)arg; (void)fmt; (void)val; return 0;
}

void sail_pointer_type_semantics_check(void);
void sail_pointer_type_semantics_check(void) {
    CellSailAudioFrameInfo af;
    void *p_pcm = (void *)0;
    af.pPcm = p_pcm;
    p_pcm = af.pPcm;
    (void)p_pcm;

    CellSailVideoFrameInfo vf;
    void *p_pic = (void *)0;
    vf.pPic = p_pic;
    p_pic = vf.pPic;
    (void)p_pic;

    CellSailSoundFrameInfo sf;
    float *p_buf = (float *)0;
    sf.pBuffer = p_buf;
    p_buf = sf.pBuffer;
    (void)p_buf;

    CellSailGraphicsFrameInfo gf;
    uint8_t *p_gbuf = (uint8_t *)0;
    gf.pBuffer = p_gbuf;
    p_gbuf = gf.pBuffer;
    (void)p_gbuf;

    CellSailSoundAdapterFuncs saf;
    saf.pMakeup = dummy_cb_makeup;
    saf.pCleanup = dummy_cb_cleanup;
    saf.pFormatChanged = dummy_cb_format_changed;
    CellSailSoundAdapterFuncMakeup p_mk = saf.pMakeup;
    CellSailSoundAdapterFuncCleanup p_cl = saf.pCleanup;
    CellSailSoundAdapterFuncFormatChanged p_fc = saf.pFormatChanged;
    (void)p_mk; (void)p_cl; (void)p_fc;

    CellSailAuInfo ai;
    ai.pAu = p_pcm;
    p_pcm = ai.pAu;

    CellSailSourceBufferItem sbi;
    sbi.pBuf = p_pcm;
    p_pcm = sbi.pBuf;

    CellSailComposerEsAudioParameter eap;
    eap.pSpecificInfo = p_pcm;
    p_pcm = eap.pSpecificInfo;

    CellSailComposerEsVideoParameter evp;
    evp.pSpecificInfo = p_pcm;
    p_pcm = evp.pSpecificInfo;

    CellSailComposerVideoAuInfo cvai;
    cvai.pAu = p_gbuf;
    p_gbuf = cvai.pAu;
    cvai.specificInfo = p_pcm;
    p_pcm = cvai.specificInfo;

    CellSailFeederFrameInfo ffi;
    ffi.pFrame = p_pcm;
    p_pcm = ffi.pFrame;
    CellSailCommand *p_cmd = (CellSailCommand *)0;
    ffi.pCommands = p_cmd;
    p_cmd = ffi.pCommands;
    (void)p_cmd;
}

#ifdef __cplusplus
extern "C" {
#endif
void sail_witness_decode_mismatch(void);
#ifdef __cplusplus
}
#endif

#define SAIL_WITNESS_CHECK(e) do { if (!(e)) sail_witness_decode_mismatch(); } while (0)

/* Byte-buffer boundary witness: simulate byte buffers filled with 32-bit big-endian
 * pointers, counts, and callback slots as populated/read by the system service.
 * Verify that field access through real records reads the exact expected 32-bit
 * addresses and counts across both ILP32 and LP64 data models.  At -O2, matching
 * assertions fold away entirely; any mismatch leaves an unresolved reference to
 * sail_witness_decode_mismatch(). */
void witness_verify_sail_boundaries(void);
void witness_verify_sail_boundaries(void) {
    /* 1. CellSailAudioFrameInfo:
     * [0..3] pPcm: 0x12345678, [4..7] status: 0x00000001, [8..15] pts: 0x0000000010000000 */
    unsigned char audio_buf[sizeof(CellSailAudioFrameInfo)];
    memset(audio_buf, 0, sizeof(audio_buf));
    audio_buf[0] = 0x12; audio_buf[1] = 0x34; audio_buf[2] = 0x56; audio_buf[3] = 0x78;
    audio_buf[7] = 0x01;
    audio_buf[12] = 0x10;

    CellSailAudioFrameInfo af;
    memcpy(&af, audio_buf, sizeof(af));
    SAIL_WITNESS_CHECK((uint32_t)af.pPcm == 0x12345678U);
    SAIL_WITNESS_CHECK(af.status == 1);
    SAIL_WITNESS_CHECK(af.pts == 0x10000000ULL);

    /* 2. CellSailSoundAdapterFuncs:
     * [0..3] pMakeup: 0x20000004, [4..7] pCleanup: 0x20000008, [8..11] pFormatChanged: 0x2000000c */
    unsigned char sound_buf[sizeof(CellSailSoundAdapterFuncs)];
    memset(sound_buf, 0, sizeof(sound_buf));
    sound_buf[0] = 0x20; sound_buf[3] = 0x04;
    sound_buf[4] = 0x20; sound_buf[7] = 0x08;
    sound_buf[8] = 0x20; sound_buf[11] = 0x0c;

    CellSailSoundAdapterFuncs saf;
    memcpy(&saf, sound_buf, sizeof(saf));
    SAIL_WITNESS_CHECK((uint32_t)saf.pMakeup == 0x20000004U);
    SAIL_WITNESS_CHECK((uint32_t)saf.pCleanup == 0x20000008U);
    SAIL_WITNESS_CHECK((uint32_t)saf.pFormatChanged == 0x2000000cU);

    /* 3. CellSailBsVideoParameter:
     * [0..3] thisSize: 80, [4..7] codecType: 2, [8..11] flags: 0x00000001 */
    unsigned char video_param_buf[sizeof(CellSailBsVideoParameter)];
    memset(video_param_buf, 0, sizeof(video_param_buf));
    video_param_buf[3] = 80;
    video_param_buf[7] = 2;
    video_param_buf[11] = 1;

    CellSailBsVideoParameter vp;
    memcpy(&vp, video_param_buf, sizeof(vp));
    SAIL_WITNESS_CHECK(vp.thisSize == 80U);
    SAIL_WITNESS_CHECK(vp.codecType == 2);
    SAIL_WITNESS_CHECK(vp.flags == 1);

    /* 4. CellSailAuInfo:
     * [0..3] pAu: 0x30000000, [4..7] size: 0x00010000, [8..11] status: 0, [12..15] sessionId: 1 */
    unsigned char au_buf[sizeof(CellSailAuInfo)];
    memset(au_buf, 0, sizeof(au_buf));
    au_buf[0] = 0x30;
    au_buf[5] = 0x01;
    au_buf[15] = 0x01;

    CellSailAuInfo ai;
    memcpy(&ai, au_buf, sizeof(ai));
    SAIL_WITNESS_CHECK((uint32_t)ai.pAu == 0x30000000U);
    SAIL_WITNESS_CHECK(ai.size == 0x00010000U);
    SAIL_WITNESS_CHECK(ai.sessionId == 1U);
}

int main(void) {
    sail_pointer_type_semantics_check();
    witness_verify_sail_boundaries();
    return 0;
}
