/*
 * PS3 Custom Toolchain - samples/fw/include/gcm/FWCellGCMWindow.h
 *
 * Cell GCM window implementation for fw samples.
 */

#ifndef PS3TC_FW_CELL_GCM_WINDOW_H
#define PS3TC_FW_CELL_GCM_WINDOW_H

#include <FWWindow.h>
#include <gcm/FWGCMConfiguration.h>
#include <gcm/FWGCMDebugFontRenderer.h>
#include <gcm/gcmutil.h>

class FWCellGCMWindow : public FWWindow {
public:
	FWCellGCMWindow(int argc,
	                char **ppArgv,
	                const FWDisplayInfo &dispInfo,
	                const FWStartupInfo &startupInfo,
	                const FWGCMConfiguration &config,
	                FWGCMDebugFontRenderer *debugFontRenderer);
	virtual ~FWCellGCMWindow();

	void init();
	bool update();
	void destroy();

	virtual void *getWindowSystemContext();
	virtual FWDisplayInfo &getDisplayInfo();
	virtual void setRenderingContext();
	virtual void clearRenderingContext();
	virtual void flip();

	CellGcmContextData *getContext() const { return mContext; }
	int getWidth() const { return mDispInfo.mWidth; }
	int getHeight() const { return mDispInfo.mHeight; }
	int getFrameIndex() const { return mFrameIndex; }

private:
	FWGCMConfiguration mConfig;
	FWDisplayInfo mDispInfo;
	FWGCMDebugFontRenderer *mDebugFontRenderer;
	void *mHostMemory;
	FWGCMDisplayBuffer mBuffers[2];
	uint32_t *mDepthBuffer;
	uint32_t mDepthOffset;
	uint32_t mDepthPitch;
	CellGcmContextData *mContext;
	int mFrameIndex;
	bool mFlipSubmitted;
	bool mGcmInit;
};

#endif /* PS3TC_FW_CELL_GCM_WINDOW_H */
