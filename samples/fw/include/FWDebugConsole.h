/*
 * PS3 Custom Toolchain - samples/fw/include/FWDebugConsole.h
 *
 * Text console facade for fw-style samples.
 */

#ifndef PS3TC_FW_DEBUG_CONSOLE_H
#define PS3TC_FW_DEBUG_CONSOLE_H

#include <FWDisplayInfo.h>
#include <FWTime.h>

#define FWDEBUGCONSOLE_NUMBER_OF_STRINGS 32
#define FWDEBUGCONSOLE_STRING_LENGTH     256
#define FWDEBUGCONSOLE_DEFAULT_TIMEOUT   5.0f

class FWDebugConsole {
public:
	static void init();
	static void shutdown();
	static void update();
	static void render();
	static void print(const char *pText);
	static void printf(const char *pText, ...);
	static void resize(const FWDisplayInfo &dispInfo);
	static void setTimeout(float secs);

	struct ConsoleString {
		char mString[FWDEBUGCONSOLE_STRING_LENGTH];
		FWTimeVal mTimeOfBirth;
		bool mIsAlive;
		int mHeight;
		int mLength;
	};

private:
	static bool sInit;
	static ConsoleString sStrings[FWDEBUGCONSOLE_NUMBER_OF_STRINGS];
	static int sCurrentString;
	static float sTimeout;
	static int sScrWidth;
	static int sScrHeight;
};

#endif /* PS3TC_FW_DEBUG_CONSOLE_H */
