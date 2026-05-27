/*
 * PS3 Custom Toolchain - samples/fw/src/FWGCMFramework.cpp
 *
 * GCM-backed fw lifecycle and debug-font renderer.
 */

#include <gcm/FWCellGCMWindow.h>
#include <gcm/FWGCMApplication.h>
#include <gcm/FWGCMCamApplication.h>
#include <gcm/FWGCMCamControlApplication.h>
#include <gcm/FWGCMDebugFontRenderer.h>
#include <gcm/gcmutil.h>

#include <cell/dbgfont.h>
#include <cell/gcm.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FWGCM_HOST_SIZE   (32u * 1024u * 1024u)
#define FWGCM_BUFFER_COUNT 2
#define FWGCM_LABEL_INDEX 255

FWGCMApplication *FWGCMApplication::spGCMApplication = 0;

static uint32_t fwColorToArgb(float r, float g, float b, float a)
{
	uint32_t ar = (uint32_t)(a < 0.0f ? 0.0f : (a > 1.0f ? 255.0f : a * 255.0f));
	uint32_t rr = (uint32_t)(r < 0.0f ? 0.0f : (r > 1.0f ? 255.0f : r * 255.0f));
	uint32_t gg = (uint32_t)(g < 0.0f ? 0.0f : (g > 1.0f ? 255.0f : g * 255.0f));
	uint32_t bb = (uint32_t)(b < 0.0f ? 0.0f : (b > 1.0f ? 255.0f : b * 255.0f));
	return (ar << 24) | (rr << 16) | (gg << 8) | bb;
}

bool fwgcmMakeDisplayBuffer(FWGCMDisplayBuffer *buffer, uint16_t width, uint16_t height, int id)
{
	if (!buffer)
		return false;

	int pitch = (int)(width * sizeof(uint32_t));
	int size = pitch * height;
	buffer->ptr = (uint32_t *)rsxMemalign(64, size);
	if (!buffer->ptr)
		return false;
	if (cellGcmAddressToOffset(buffer->ptr, &buffer->offset) != 0)
		return false;
	if (cellGcmSetDisplayBuffer((uint8_t)id, buffer->offset, pitch, width, height) != 0)
		return false;

	buffer->width = width;
	buffer->height = height;
	buffer->id = id;
	return true;
}

void fwgcmSetRenderTarget(CellGcmContextData *ctx, FWGCMDisplayBuffer *buffer, uint32_t depthOffset, uint32_t depthPitch)
{
	if (!ctx || !buffer)
		return;

	CellGcmSurface surface;
	memset(&surface, 0, sizeof(surface));
	surface.colorFormat = GCM_SURFACE_X8R8G8B8;
	surface.colorTarget = GCM_SURFACE_TARGET_0;
	surface.colorLocation[0] = GCM_LOCATION_RSX;
	surface.colorOffset[0] = buffer->offset;
	surface.colorPitch[0] = depthPitch;
	surface.colorLocation[1] = GCM_LOCATION_RSX;
	surface.colorLocation[2] = GCM_LOCATION_RSX;
	surface.colorLocation[3] = GCM_LOCATION_RSX;
	surface.colorPitch[1] = 64;
	surface.colorPitch[2] = 64;
	surface.colorPitch[3] = 64;
	surface.depthFormat = GCM_SURFACE_ZETA_Z16;
	surface.depthLocation = GCM_LOCATION_RSX;
	surface.depthOffset = depthOffset;
	surface.depthPitch = depthPitch;
	surface.type = GCM_TEXTURE_LINEAR;
	surface.antiAlias = GCM_SURFACE_CENTER_1;
	surface.width = buffer->width;
	surface.height = buffer->height;
	surface.x = 0;
	surface.y = 0;
	cellGcmSetSurface(ctx, &surface);

	float scale[4] = { buffer->width / 2.0f, -(float)buffer->height / 2.0f, 0.5f, 0.0f };
	float offset[4] = { buffer->width / 2.0f,  (float)buffer->height / 2.0f, 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, buffer->width, buffer->height, 0.0f, 1.0f, scale, offset);
	cellGcmSetScissor(ctx, 0, 0, buffer->width, buffer->height);
}

void fwgcmWaitFlip()
{
	while (cellGcmGetFlipStatus() != 0)
		usleep(200);
	cellGcmResetFlipStatus();
}

bool fwgcmDoFlip(CellGcmContextData *ctx, uint8_t bufferId)
{
	if (!ctx)
		return false;
	if (cellGcmSetFlip(ctx, bufferId) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
		return true;
	}
	return false;
}

void fwgcmWaitIdle(CellGcmContextData *ctx)
{
	if (!ctx)
		return;
	static uint32_t sIdleLabel = 0;
	uint32_t label = ++sIdleLabel;
	cellGcmSetWriteBackEndLabel(ctx, FWGCM_LABEL_INDEX, label);
	cellGcmFlush(ctx);
	while (*(volatile uint32_t *)cellGcmGetLabelAddress(FWGCM_LABEL_INDEX) != label)
		usleep(30);
}

FWGCMDebugFontRenderer::FWGCMDebugFontRenderer()
	: FWDebugFontRenderer()
	, mBuffer(0)
	, mBufferSize(0)
	, mWidth(1280)
	, mHeight(720)
	, mInitialized(false)
{
}

FWGCMDebugFontRenderer::~FWGCMDebugFontRenderer()
{
	shutdown();
}

bool FWGCMDebugFontRenderer::initWithScreen(int width, int height)
{
	mWidth = width > 0 ? width : 1280;
	mHeight = height > 0 ? height : 720;
	mBufferSize = CELL_DBGFONT_FRAGMENT_SIZE + CELL_DBGFONT_TEXTURE_SIZE + 9000u * CELL_DBGFONT_VERTEX_SIZE;
	mBuffer = rsxMemalign(128, mBufferSize);
	if (!mBuffer)
		return false;

	CellDbgFontConfigGcm cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.localBufAddr = (sys_addr_t)(uintptr_t)mBuffer;
	cfg.localBufSize = mBufferSize;
	cfg.screenWidth = (uint16_t)mWidth;
	cfg.screenHeight = (uint16_t)mHeight;
	cfg.option = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;
	int rc = cellDbgFontInitGcm(&cfg);
	mInitialized = (rc == 0);
	printf("  cellDbgFontInitGcm -> 0x%08x\n", (unsigned)rc);
	return mInitialized;
}

void FWGCMDebugFontRenderer::init()
{
}

void FWGCMDebugFontRenderer::shutdown()
{
	if (mInitialized) {
		cellDbgFontExitGcm();
		mInitialized = false;
	}
	mBuffer = 0;
	mBufferSize = 0;
}

void FWGCMDebugFontRenderer::printStart(float r, float g, float b, float a)
{
	FWDebugFontRenderer::printStart(r, g, b, a);
}

void FWGCMDebugFontRenderer::printPass(FWDebugFont::Position *pPositions,
                                       FWDebugFont::TexCoord *pTexCoords,
                                       FWDebugFont::Color *pColors,
                                       int numVerts)
{
	(void)pPositions;
	(void)pTexCoords;
	(void)pColors;
	(void)numVerts;
}

void FWGCMDebugFontRenderer::printEnd()
{
}

bool FWGCMDebugFontRenderer::printText(const char *pText,
                                       int numChars,
                                       int x,
                                       int y,
                                       float r,
                                       float g,
                                       float b,
                                       float a)
{
	if (!mInitialized || !pText || numChars <= 0)
		return false;

	char buffer[1024];
	int count = numChars < (int)sizeof(buffer) - 1 ? numChars : (int)sizeof(buffer) - 1;
	memcpy(buffer, pText, (size_t)count);
	buffer[count] = '\0';
	/* cellDbgFontPuts uses normalized [0,1] screen coordinates. */
	float nx = mWidth ? (float)x / (float)mWidth : 0.0f;
	float ny = mHeight ? (float)y / (float)mHeight : 0.0f;
	cellDbgFontPuts(nx, ny, 1.0f, fwColorToArgb(r, g, b, a), buffer);
	return true;
}

void FWGCMDebugFontRenderer::draw()
{
	if (mInitialized)
		cellDbgFontDrawGcm();
}

FWCellGCMWindow::FWCellGCMWindow(int argc,
                                 char **ppArgv,
                                 const FWDisplayInfo &dispInfo,
                                 const FWStartupInfo &startupInfo,
                                 const FWGCMConfiguration &config,
                                 FWGCMDebugFontRenderer *debugFontRenderer)
	: FWWindow(argc, ppArgv, dispInfo, startupInfo)
	, mConfig(config)
	, mDispInfo(dispInfo)
	, mDebugFontRenderer(debugFontRenderer)
	, mHostMemory(0)
	, mDepthBuffer(0)
	, mDepthOffset(0)
	, mDepthPitch(0)
	, mContext(0)
	, mFrameIndex(0)
	, mFlipSubmitted(false)
	, mGcmInit(false)
{
	memset(mBuffers, 0, sizeof(mBuffers));
}

FWCellGCMWindow::~FWCellGCMWindow()
{
	destroy();
}

void FWCellGCMWindow::init()
{
	mHostMemory = memalign(1024 * 1024, FWGCM_HOST_SIZE);
	if (!mHostMemory) {
		printf("  FWCellGCMWindow: host memory allocation failed\n");
		return;
	}
	int rc = cellGcmInit(mConfig.mDefaultCBSize, FWGCM_HOST_SIZE, mHostMemory);
	printf("  cellGcmInit -> 0x%08x\n", (unsigned)rc);
	if (rc != 0)
		return;

	videoState state;
	videoConfiguration vcfg;
	videoResolution res;
	int vrc = videoGetState(0, 0, &state);
	if (vrc != 0) {
		printf("  videoGetState -> 0x%08x (failed)\n", (unsigned)vrc);
		return;
	}
	if (state.state != 0) {
		printf("  videoGetState -> 0x%08x (output state=%u)\n",
		       (unsigned)vrc, (unsigned)state.state);
		return;
	}
	vrc = videoGetResolution(state.displayMode.resolution, &res);
	if (vrc != 0) {
		printf("  videoGetResolution -> 0x%08x (failed)\n", (unsigned)vrc);
		return;
	}

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch = res.width * sizeof(uint32_t);
	vcfg.aspect = state.displayMode.aspect;

	mContext = CELL_GCM_CURRENT;
	fwgcmWaitIdle(mContext);
	vrc = videoConfigure(0, &vcfg, 0, 0);
	if (vrc != 0) {
		printf("  videoConfigure -> 0x%08x (failed)\n", (unsigned)vrc);
		return;
	}
	cellGcmSetFlipMode(mDispInfo.mVSync ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);

	mDispInfo.mWidth = res.width;
	mDispInfo.mHeight = res.height;
	mDepthPitch = res.width * sizeof(uint32_t);
	mDepthBuffer = (uint32_t *)rsxMemalign(64, res.height * mDepthPitch * 2);
	if (!mDepthBuffer)
		return;
	cellGcmAddressToOffset(mDepthBuffer, &mDepthOffset);
	cellGcmResetFlipStatus();

	for (int i = 0; i < FWGCM_BUFFER_COUNT; ++i) {
		if (!fwgcmMakeDisplayBuffer(&mBuffers[i], res.width, res.height, i))
			return;
	}

	FWDebugFont::setRenderer(mDebugFontRenderer);
	if (mDebugFontRenderer && !mDebugFontRenderer->initWithScreen(res.width, res.height))
		return;

	mGcmInit = true;
	FWWindow::init();
}

bool FWCellGCMWindow::update()
{
	FWWindow::update();
	return true;
}

void FWCellGCMWindow::destroy()
{
	FWWindow::destroy();
	if (mDebugFontRenderer)
		mDebugFontRenderer->shutdown();
	if (mHostMemory) {
		free(mHostMemory);
		mHostMemory = 0;
	}
	mGcmInit = false;
}

void *FWCellGCMWindow::getWindowSystemContext()
{
	return mContext;
}

FWDisplayInfo &FWCellGCMWindow::getDisplayInfo()
{
	return mDispInfo;
}

void FWCellGCMWindow::setRenderingContext()
{
	fwgcmSetRenderTarget(mContext, &mBuffers[mFrameIndex], mDepthOffset, mDepthPitch);
}

void FWCellGCMWindow::clearRenderingContext()
{
	if (mDebugFontRenderer)
		mDebugFontRenderer->draw();
}

void FWCellGCMWindow::flip()
{
	if (mFlipSubmitted)
		fwgcmWaitFlip();
	if (fwgcmDoFlip(mContext, (uint8_t)mBuffers[mFrameIndex].id))
		mFlipSubmitted = true;
	mFrameIndex = (mFrameIndex + 1) % FWGCM_BUFFER_COUNT;
}

FWGCMApplication::FWGCMApplication()
	: FWApplication()
	, mWindow(0)
	, mExitRequested(false)
	, mFrameLimit(60)
	, mFrameCount(0)
{
	FWASSERT(spGCMApplication == 0);
	spGCMApplication = this;
}

FWGCMApplication::~FWGCMApplication()
{
	if (spGCMApplication == this)
		spGCMApplication = 0;
}

bool FWGCMApplication::onInit(int argc, char **ppArgv)
{
	return FWApplication::onInit(argc, ppArgv);
}

void FWGCMApplication::onShutdown()
{
	FWApplication::onShutdown();
}

bool FWGCMApplication::onUpdate()
{
	return FWApplication::onUpdate();
}

void FWGCMApplication::onRender()
{
}

void FWGCMApplication::onSize(const FWDisplayInfo &dispInfo)
{
	mDispInfo = dispInfo;
}

int FWGCMApplication::run(int argc, char **ppArgv)
{
	mWindow = new FWCellGCMWindow(argc, ppArgv, mDispInfo, mStartupInfo, mConfiguration, &mDebugFontRenderer);
	if (!mWindow)
		return 1;
	mWindow->init();
	while (!mExitRequested && mFrameCount < mFrameLimit) {
		mWindow->update();
		mWindow->render();
		mWindow->flip();
		++mFrameCount;
	}
	mWindow->destroy();
	delete mWindow;
	mWindow = 0;
	return 0;
}

void FWGCMApplication::requestExit()
{
	mExitRequested = true;
}

void FWGCMApplication::setFrameLimit(int frameLimit)
{
	mFrameLimit = frameLimit > 0 ? frameLimit : 1;
}

FWGCMApplication *FWGCMApplication::getGCMApplication()
{
	FWASSERT(spGCMApplication != 0);
	return spGCMApplication;
}

FWGCMCamApplication::FWGCMCamApplication()
	: FWGCMApplication()
	, mViewMatrix(Matrix4::identity())
	, mProjectionMatrix(Matrix4::identity())
	, mCameraPosition(0.0f, 0.0f, 5.0f)
	, mCameraTarget(0.0f, 0.0f, 0.0f)
{
}

FWGCMCamApplication::~FWGCMCamApplication()
{
}

bool FWGCMCamApplication::onUpdate()
{
	return FWGCMApplication::onUpdate();
}

FWGCMCamControlApplication::FWGCMCamControlApplication()
	: FWGCMCamApplication()
	, mLastMoveX(0.0f)
	, mLastMoveZ(0.0f)
	, mLastYaw(0.0f)
	, mLastPitch(0.0f)
{
}

FWGCMCamControlApplication::~FWGCMCamControlApplication()
{
}

bool FWGCMCamControlApplication::onUpdate()
{
	mLastMoveX = 0.0f;
	mLastMoveZ = 0.0f;
	mLastYaw = 0.0f;
	mLastPitch = 0.0f;
	return FWGCMCamApplication::onUpdate();
}
