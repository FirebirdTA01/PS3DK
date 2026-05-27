/*
 * PS3 Custom Toolchain - samples/fw/include/gcm/FWGCMDebugFontRenderer.h
 *
 * libdbgfont-backed debug-font renderer for fw GCM samples.
 */

#ifndef PS3TC_FW_GCM_DEBUG_FONT_RENDERER_H
#define PS3TC_FW_GCM_DEBUG_FONT_RENDERER_H

#include <FWDebugFontRenderer.h>

class FWGCMDebugFontRenderer : public FWDebugFontRenderer {
public:
	FWGCMDebugFontRenderer();
	virtual ~FWGCMDebugFontRenderer();

	bool initWithScreen(int width, int height);
	virtual void init();
	virtual void shutdown();
	virtual void printStart(float r, float g, float b, float a);
	virtual void printPass(FWDebugFont::Position *pPositions,
	                       FWDebugFont::TexCoord *pTexCoords,
	                       FWDebugFont::Color *pColors,
	                       int numVerts);
	virtual void printEnd();
	virtual bool printText(const char *pText,
	                       int numChars,
	                       int x,
	                       int y,
	                       float r,
	                       float g,
	                       float b,
	                       float a);
	void draw();
	bool isInitialized() const { return mInitialized; }

private:
	void *mBuffer;
	unsigned int mBufferSize;
	int mWidth;
	int mHeight;
	bool mInitialized;
};

#endif /* PS3TC_FW_GCM_DEBUG_FONT_RENDERER_H */
