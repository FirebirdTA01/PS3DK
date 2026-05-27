/*
 * PS3 Custom Toolchain - samples/fw/include/FWDebugFont.h
 *
 * Debug text facade for the sample fw layer.
 */

#ifndef PS3TC_FW_DEBUG_FONT_H
#define PS3TC_FW_DEBUG_FONT_H

#include <stdarg.h>

#define FWDEBUGFONT_GLYPH_WIDTH  8
#define FWDEBUGFONT_GLYPH_HEIGHT 8

class FWDebugFontRenderer;

class FWDebugFont {
public:
	struct Position {
		float mX;
		float mY;
		float mZ;
	};

	struct TexCoord {
		float mS;
		float mT;
	};

	struct Color {
		float mR;
		float mG;
		float mB;
		float mA;
	};

	static void init();
	static void shutdown();
	static void setRenderer(FWDebugFontRenderer *pRenderer);
	static FWDebugFontRenderer *getRenderer();

	static void setPosition(int x, int y);
	static void setColor(float r, float g, float b, float a);
	static void setScreenRes(int x, int y);
	static void setSafeArea(int left, int right, int top, int bottom);
	static void getSafeArea(int *pLeft, int *pRight, int *pTop, int *pBottom);

	static int getGlyphWidth();
	static int getGlyphHeight();
	static void getExtents(const char *pText, int *piWidth, int *piHeight, int iScrWidth);

	static void print(const char *pText);
	static void print(const char *pText, int iNumChars);
	static void printf(const char *pText, ...);
	static void vprintf(const char *pText, va_list args);

	static float calcPos(int pos, int dim);
	static float calcS0(unsigned char c);
	static float calcS1(unsigned char c);
	static float calcT0(unsigned char c);
	static float calcT1(unsigned char c);
	static bool getPrintable(char c);

private:
	static int sXPos;
	static int sYPos;
	static int sXRes;
	static int sYRes;
	static int sLeftSafe;
	static int sRightSafe;
	static int sTopSafe;
	static int sBottomSafe;
	static float sR;
	static float sG;
	static float sB;
	static float sA;
	static FWDebugFontRenderer *spRenderer;
};

#endif /* PS3TC_FW_DEBUG_FONT_H */
