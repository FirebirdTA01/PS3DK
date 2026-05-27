/*
 * PS3 Custom Toolchain - samples/fw/include/FWDisplayInfo.h
 *
 * Display configuration container for fw-style samples.
 */

#ifndef PS3TC_FW_DISPLAY_INFO_H
#define PS3TC_FW_DISPLAY_INFO_H

#define FWDISPLAYINFO_DEFAULT_MODE                   FWDisplayInfo::DisplayMode_720p
#define FWDISPLAYINFO_DEFAULT_WIDTH                  1280
#define FWDISPLAYINFO_DEFAULT_HEIGHT                 720
#define FWDISPLAYINFO_DEFAULT_COLOR                  32
#define FWDISPLAYINFO_DEFAULT_ALPHA                  8
#define FWDISPLAYINFO_DEFAULT_DEPTH                  24
#define FWDISPLAYINFO_DEFAULT_STENCIL                8
#define FWDISPLAYINFO_DEFAULT_ANTIALIAS              false
#define FWDISPLAYINFO_DEFAULT_VSYNC                  true
#define FWDISPLAYINFO_DEFAULT_PERSISTENT_MEMORY_SIZE (160 << 20)
#define FWDISPLAYINFO_DEFAULT_PSGL_RAW_SPUS          1
#define FWDISPLAYINFO_DEFAULT_HOST_MEMORY_SIZE       0

class FWDisplayInfo {
public:
	enum DisplayMode {
		DisplayMode_VGA,
		DisplayMode_480i,
		DisplayMode_480p,
		DisplayMode_576i,
		DisplayMode_576p,
		DisplayMode_720p,
		DisplayMode_1080i,
		DisplayMode_1080p,
		DisplayMode_WXGA_720p,
		DisplayMode_SXGA_720p,
		DisplayMode_WUXGA_1080p,
	};

	FWDisplayInfo()
		: mDisplayMode(FWDISPLAYINFO_DEFAULT_MODE),
		  mWidth(FWDISPLAYINFO_DEFAULT_WIDTH),
		  mHeight(FWDISPLAYINFO_DEFAULT_HEIGHT),
		  mColorBits(FWDISPLAYINFO_DEFAULT_COLOR),
		  mAlphaBits(FWDISPLAYINFO_DEFAULT_ALPHA),
		  mDepthBits(FWDISPLAYINFO_DEFAULT_DEPTH),
		  mStencilBits(FWDISPLAYINFO_DEFAULT_STENCIL),
		  mAntiAlias(FWDISPLAYINFO_DEFAULT_ANTIALIAS),
		  mVSync(FWDISPLAYINFO_DEFAULT_VSYNC),
		  mWideScreen(true)
	{
	}

	~FWDisplayInfo() {}

	DisplayMode mDisplayMode;
	int         mWidth;
	int         mHeight;
	int         mColorBits;
	int         mAlphaBits;
	int         mDepthBits;
	int         mStencilBits;
	bool        mAntiAlias;
	bool        mVSync;
	bool        mWideScreen;
};

#endif /* PS3TC_FW_DISPLAY_INFO_H */
