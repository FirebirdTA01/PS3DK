/*
 * PS3 Custom Toolchain - samples/fw/include/gcm/gcmutil.h
 *
 * Small GCM helpers shared by fw GCM samples.
 */

#ifndef PS3TC_FW_GCM_UTIL_H
#define PS3TC_FW_GCM_UTIL_H

#include <stdint.h>
#include <cell/gcm.h>

struct FWGCMDisplayBuffer {
	uint32_t *ptr;
	uint32_t offset;
	uint16_t width;
	uint16_t height;
	int id;
};

bool fwgcmMakeDisplayBuffer(FWGCMDisplayBuffer *buffer, uint16_t width, uint16_t height, int id);
void fwgcmSetRenderTarget(CellGcmContextData *ctx, FWGCMDisplayBuffer *buffer, uint32_t depthOffset, uint32_t depthPitch);
void fwgcmWaitFlip();
bool fwgcmDoFlip(CellGcmContextData *ctx, uint8_t bufferId);
void fwgcmWaitIdle(CellGcmContextData *ctx);

#endif /* PS3TC_FW_GCM_UTIL_H */
