/*
 * PS3 Custom Toolchain - samples/fw/src/FWNullFramework.cpp
 *
 * No-op fw lifecycle and text facade implementation.
 */

#include <FWApplication.h>
#include <FWDebugConsole.h>
#include <FWDebugFont.h>
#include <FWDebugFontRenderer.h>
#include <FWNullApplication.h>
#include <FWNullDebugFontRenderer.h>
#include <FWNullWindow.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

FWApplication *FWApplication::spApplication = 0;
FWWindow *FWWindow::spWindow = 0;

int FWDebugFont::sXPos = 0;
int FWDebugFont::sYPos = 0;
int FWDebugFont::sXRes = 1280;
int FWDebugFont::sYRes = 720;
int FWDebugFont::sLeftSafe = 0;
int FWDebugFont::sRightSafe = 0;
int FWDebugFont::sTopSafe = 0;
int FWDebugFont::sBottomSafe = 0;
float FWDebugFont::sR = 1.0f;
float FWDebugFont::sG = 1.0f;
float FWDebugFont::sB = 1.0f;
float FWDebugFont::sA = 1.0f;
FWDebugFontRenderer *FWDebugFont::spRenderer = 0;

float FWDebugFontRenderer::sR = 1.0f;
float FWDebugFontRenderer::sG = 1.0f;
float FWDebugFontRenderer::sB = 1.0f;
float FWDebugFontRenderer::sA = 1.0f;

bool FWDebugConsole::sInit = false;
FWDebugConsole::ConsoleString FWDebugConsole::sStrings[FWDEBUGCONSOLE_NUMBER_OF_STRINGS];
int FWDebugConsole::sCurrentString = 0;
float FWDebugConsole::sTimeout = FWDEBUGCONSOLE_DEFAULT_TIMEOUT;
int FWDebugConsole::sScrWidth = 1280;
int FWDebugConsole::sScrHeight = 720;

static void fwClearConsoleString(FWDebugConsole::ConsoleString &str)
{
	str.mString[0] = '\0';
	str.mTimeOfBirth = FWTimeVal(0.0);
	str.mIsAlive = false;
	str.mHeight = 0;
	str.mLength = 0;
}

FWApplication::FWApplication()
{
	FWASSERT(spApplication == 0);
	spApplication = this;
}

FWApplication::~FWApplication()
{
	if (spApplication == this)
		spApplication = 0;
}

bool FWApplication::onInit(int argc, char **ppArgv)
{
	(void)argc;
	(void)ppArgv;
	return true;
}

void FWApplication::onShutdown()
{
}

bool FWApplication::onUpdate()
{
	return true;
}

FWWindow::FWWindow(int argc, char **ppArgv, const FWDisplayInfo &dispInfo, const FWStartupInfo &startupInfo)
	: mUseDebugConsole(startupInfo.mUseDebugConsole)
	, mInit(false)
	, mUpdate(false)
	, mArgc(argc)
	, mppArgv(ppArgv)
	, mpArgFile(0)
{
	(void)dispInfo;
	FWASSERT(spWindow == 0);
	spWindow = this;
}

FWWindow::~FWWindow()
{
	if (spWindow == this)
		spWindow = 0;
}

void FWWindow::init()
{
	FWDebugFont::init();
	if (mUseDebugConsole)
		FWDebugConsole::init();
	mInit = true;
	mUpdate = FWApplication::getApplication()->onInit(mArgc, mppArgv);
}

void FWWindow::update()
{
	if (!mInit || !mUpdate)
		return;
	if (mUseDebugConsole)
		FWDebugConsole::update();
	mUpdate = FWApplication::getApplication()->onUpdate();
}

void FWWindow::render()
{
	if (!mInit || !mUpdate)
		return;
	setRenderingContext();
	FWApplication::getApplication()->onRender();
	if (mUseDebugConsole)
		FWDebugConsole::render();
	clearRenderingContext();
}

void FWWindow::destroy()
{
	if (!mInit)
		return;
	FWApplication::getApplication()->onShutdown();
	if (mUseDebugConsole)
		FWDebugConsole::shutdown();
	FWDebugFont::shutdown();
	mInit = false;
	mUpdate = false;
}

void FWWindow::parseArgumentsFile(const char *pArgumentsFile)
{
	mpArgFile = const_cast<char *>(pArgumentsFile);
}

void FWWindow::resize(int width, int height)
{
	FWDisplayInfo &dispInfo = getDisplayInfo();
	dispInfo.mWidth = width;
	dispInfo.mHeight = height;
	FWDebugFont::setScreenRes(width, height);
	FWDebugConsole::resize(dispInfo);
	FWApplication::getApplication()->onSize(dispInfo);
}

FWNullApplication::FWNullApplication()
	: FWApplication()
{
	FWDebugFont::setRenderer(&mFontRenderer);
}

FWNullApplication::~FWNullApplication()
{
	if (FWDebugFont::getRenderer() == &mFontRenderer)
		FWDebugFont::setRenderer(0);
}

bool FWNullApplication::onInit(int argc, char **ppArgv)
{
	return FWApplication::onInit(argc, ppArgv);
}

void FWNullApplication::onShutdown()
{
	FWApplication::onShutdown();
}

bool FWNullApplication::onUpdate()
{
	return FWApplication::onUpdate();
}

void FWNullApplication::onRender()
{
}

void FWNullApplication::onSize(const FWDisplayInfo &dispInfo)
{
	mDispInfo = dispInfo;
}

FWNullWindow::FWNullWindow(int argc, char **ppArgv, const FWDisplayInfo &dispInfo, const FWStartupInfo &startupInfo)
	: FWWindow(argc, ppArgv, dispInfo, startupInfo)
	, mDispInfo(dispInfo)
{
}

FWNullWindow::~FWNullWindow()
{
}

bool FWNullWindow::update()
{
	FWWindow::update();
	FWWindow::render();
	flip();
	return true;
}

void FWNullWindow::destroy()
{
	FWWindow::destroy();
}

void *FWNullWindow::getWindowSystemContext()
{
	return 0;
}

FWDisplayInfo &FWNullWindow::getDisplayInfo()
{
	return mDispInfo;
}

void FWNullWindow::setRenderingContext()
{
}

void FWNullWindow::clearRenderingContext()
{
}

void FWNullWindow::flip()
{
}

FWDebugFontRenderer::FWDebugFontRenderer()
{
}

FWDebugFontRenderer::~FWDebugFontRenderer()
{
}

void FWDebugFontRenderer::init()
{
}

void FWDebugFontRenderer::shutdown()
{
}

void FWDebugFontRenderer::printStart(float r, float g, float b, float a)
{
	sR = r;
	sG = g;
	sB = b;
	sA = a;
}

void FWDebugFontRenderer::printPass(FWDebugFont::Position *pPositions,
                                    FWDebugFont::TexCoord *pTexCoords,
                                    FWDebugFont::Color *pColors,
                                    int numVerts)
{
	(void)pPositions;
	(void)pTexCoords;
	(void)pColors;
	(void)numVerts;
}

void FWDebugFontRenderer::printEnd()
{
}

bool FWDebugFontRenderer::printText(const char *pText,
                                    int numChars,
                                    int x,
                                    int y,
                                    float r,
                                    float g,
                                    float b,
                                    float a)
{
	(void)pText;
	(void)numChars;
	(void)x;
	(void)y;
	(void)r;
	(void)g;
	(void)b;
	(void)a;
	return false;
}

unsigned char *FWDebugFontRenderer::getFontData()
{
	return 0;
}

FWNullDebugFontRenderer::FWNullDebugFontRenderer()
{
}

FWNullDebugFontRenderer::~FWNullDebugFontRenderer()
{
}

void FWNullDebugFontRenderer::init()
{
}

void FWNullDebugFontRenderer::shutdown()
{
}

void FWNullDebugFontRenderer::printStart(float r, float g, float b, float a)
{
	FWDebugFontRenderer::printStart(r, g, b, a);
}

void FWNullDebugFontRenderer::printPass(FWDebugFont::Position *pPositions,
                                        FWDebugFont::TexCoord *pTexCoords,
                                        FWDebugFont::Color *pColors,
                                        int numVerts)
{
	(void)pPositions;
	(void)pTexCoords;
	(void)pColors;
	(void)numVerts;
}

void FWNullDebugFontRenderer::printEnd()
{
}

void FWDebugFont::init()
{
	if (spRenderer)
		spRenderer->init();
}

void FWDebugFont::shutdown()
{
	if (spRenderer)
		spRenderer->shutdown();
}

void FWDebugFont::setRenderer(FWDebugFontRenderer *pRenderer)
{
	spRenderer = pRenderer;
}

FWDebugFontRenderer *FWDebugFont::getRenderer()
{
	return spRenderer;
}

void FWDebugFont::setPosition(int x, int y)
{
	sXPos = x;
	sYPos = y;
}

void FWDebugFont::setColor(float r, float g, float b, float a)
{
	sR = r;
	sG = g;
	sB = b;
	sA = a;
}

void FWDebugFont::setScreenRes(int x, int y)
{
	sXRes = x;
	sYRes = y;
}

void FWDebugFont::setSafeArea(int left, int right, int top, int bottom)
{
	sLeftSafe = left;
	sRightSafe = right;
	sTopSafe = top;
	sBottomSafe = bottom;
}

void FWDebugFont::getSafeArea(int *pLeft, int *pRight, int *pTop, int *pBottom)
{
	if (pLeft)
		*pLeft = sLeftSafe;
	if (pRight)
		*pRight = sRightSafe;
	if (pTop)
		*pTop = sTopSafe;
	if (pBottom)
		*pBottom = sBottomSafe;
}

int FWDebugFont::getGlyphWidth()
{
	return FWDEBUGFONT_GLYPH_WIDTH;
}

int FWDebugFont::getGlyphHeight()
{
	return FWDEBUGFONT_GLYPH_HEIGHT;
}

void FWDebugFont::getExtents(const char *pText, int *piWidth, int *piHeight, int iScrWidth)
{
	int width = 0;
	int maxWidth = 0;
	int height = FWDEBUGFONT_GLYPH_HEIGHT;
	const char *p = pText ? pText : "";
	for (; *p; ++p) {
		if (*p == '\n' || (iScrWidth > 0 && width + FWDEBUGFONT_GLYPH_WIDTH > iScrWidth)) {
			if (width > maxWidth)
				maxWidth = width;
			width = 0;
			height += FWDEBUGFONT_GLYPH_HEIGHT;
			if (*p == '\n')
				continue;
		}
		width += FWDEBUGFONT_GLYPH_WIDTH;
	}
	if (width > maxWidth)
		maxWidth = width;
	if (piWidth)
		*piWidth = maxWidth;
	if (piHeight)
		*piHeight = height;
}

void FWDebugFont::print(const char *pText)
{
	print(pText, pText ? (int)strlen(pText) : 0);
}

void FWDebugFont::print(const char *pText, int iNumChars)
{
	if (!pText || iNumChars <= 0)
		return;
	if (spRenderer && spRenderer->printText(pText, iNumChars, sXPos, sYPos, sR, sG, sB, sA)) {
		sYPos += FWDEBUGFONT_GLYPH_HEIGHT;
		return;
	}
	if (spRenderer) {
		spRenderer->printStart(sR, sG, sB, sA);
		spRenderer->printEnd();
	}
	::printf("%.*s", iNumChars, pText);
}

void FWDebugFont::printf(const char *pText, ...)
{
	va_list args;
	va_start(args, pText);
	vprintf(pText, args);
	va_end(args);
}

void FWDebugFont::vprintf(const char *pText, va_list args)
{
	char buffer[1024];
	::vsnprintf(buffer, sizeof(buffer), pText ? pText : "", args);
	print(buffer);
}

float FWDebugFont::calcPos(int pos, int dim)
{
	return dim ? ((2.0f * (float)pos) / (float)dim) - 1.0f : 0.0f;
}

float FWDebugFont::calcS0(unsigned char c)
{
	return (float)(c & 15) / 16.0f;
}

float FWDebugFont::calcS1(unsigned char c)
{
	return ((float)(c & 15) + 1.0f) / 16.0f;
}

float FWDebugFont::calcT0(unsigned char c)
{
	return (float)(c >> 4) / 16.0f;
}

float FWDebugFont::calcT1(unsigned char c)
{
	return ((float)(c >> 4) + 1.0f) / 16.0f;
}

bool FWDebugFont::getPrintable(char c)
{
	return c >= 32 && c < 127;
}

void FWDebugConsole::init()
{
	for (int i = 0; i < FWDEBUGCONSOLE_NUMBER_OF_STRINGS; ++i)
		fwClearConsoleString(sStrings[i]);
	sCurrentString = 0;
	sTimeout = FWDEBUGCONSOLE_DEFAULT_TIMEOUT;
	sInit = true;
}

void FWDebugConsole::shutdown()
{
	sInit = false;
}

void FWDebugConsole::update()
{
	if (!sInit)
		return;
	FWTimeVal now = FWTime::getCurrentTime();
	for (int i = 0; i < FWDEBUGCONSOLE_NUMBER_OF_STRINGS; ++i) {
		if (sStrings[i].mIsAlive && (double)(now - sStrings[i].mTimeOfBirth) > (double)sTimeout)
			sStrings[i].mIsAlive = false;
	}
}

void FWDebugConsole::render()
{
	if (!sInit)
		return;
	for (int i = 0; i < FWDEBUGCONSOLE_NUMBER_OF_STRINGS; ++i) {
		if (!sStrings[i].mIsAlive)
			continue;
		FWDebugFont::setPosition(0, i * FWDEBUGFONT_GLYPH_HEIGHT);
		FWDebugFont::print(sStrings[i].mString, sStrings[i].mLength);
	}
}

void FWDebugConsole::print(const char *pText)
{
	if (!sInit)
		init();
	ConsoleString &str = sStrings[sCurrentString];
	fwClearConsoleString(str);
	strncpy(str.mString, pText ? pText : "", sizeof(str.mString) - 1);
	str.mString[sizeof(str.mString) - 1] = '\0';
	str.mLength = (int)strlen(str.mString);
	str.mHeight = FWDEBUGFONT_GLYPH_HEIGHT;
	str.mTimeOfBirth = FWTime::getCurrentTime();
	str.mIsAlive = true;
	sCurrentString = (sCurrentString + 1) % FWDEBUGCONSOLE_NUMBER_OF_STRINGS;
	::printf("%s", str.mString);
}

void FWDebugConsole::printf(const char *pText, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, pText);
	::vsnprintf(buffer, sizeof(buffer), pText ? pText : "", args);
	va_end(args);
	print(buffer);
}

void FWDebugConsole::resize(const FWDisplayInfo &dispInfo)
{
	sScrWidth = dispInfo.mWidth;
	sScrHeight = dispInfo.mHeight;
}

void FWDebugConsole::setTimeout(float secs)
{
	sTimeout = secs;
}
