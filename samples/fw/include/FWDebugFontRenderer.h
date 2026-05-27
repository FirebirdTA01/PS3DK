/*
 * PS3 Custom Toolchain - samples/fw/include/FWDebugFontRenderer.h
 *
 * Debug-font renderer interface for the sample fw layer.
 */

#ifndef PS3TC_FW_DEBUG_FONT_RENDERER_H
#define PS3TC_FW_DEBUG_FONT_RENDERER_H

#include <FWDebugFont.h>

class FWDebugFontRenderer {
public:
	FWDebugFontRenderer();
	virtual ~FWDebugFontRenderer();

	virtual void init();
	virtual void shutdown();
	virtual void printStart(float r, float g, float b, float a);
	virtual void printPass(FWDebugFont::Position *pPositions,
	                       FWDebugFont::TexCoord *pTexCoords,
	                       FWDebugFont::Color *pColors,
	                       int numVerts);
	virtual void printEnd();

	static unsigned char *getFontData();

	static float sR;
	static float sG;
	static float sB;
	static float sA;
};

#endif /* PS3TC_FW_DEBUG_FONT_RENDERER_H */
