/*
 * PS3 Custom Toolchain - samples/fw/include/FWNullDebugFontRenderer.h
 *
 * No-op debug-font renderer for the sample fw layer.
 */

#ifndef PS3TC_FW_NULL_DEBUG_FONT_RENDERER_H
#define PS3TC_FW_NULL_DEBUG_FONT_RENDERER_H

#include <FWDebugFontRenderer.h>

class FWNullDebugFontRenderer : public FWDebugFontRenderer {
public:
	FWNullDebugFontRenderer();
	virtual ~FWNullDebugFontRenderer();

	virtual void init();
	virtual void shutdown();
	virtual void printStart(float r, float g, float b, float a);
	virtual void printPass(FWDebugFont::Position *pPositions,
	                       FWDebugFont::TexCoord *pTexCoords,
	                       FWDebugFont::Color *pColors,
	                       int numVerts);
	virtual void printEnd();
};

#endif /* PS3TC_FW_NULL_DEBUG_FONT_RENDERER_H */
