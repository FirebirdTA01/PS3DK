/*
 * PS3 Custom Toolchain - samples/fw/include/FWStartupInfo.h
 *
 * Startup configuration container for fw-style samples.
 */

#ifndef PS3TC_FW_STARTUP_INFO_H
#define PS3TC_FW_STARTUP_INFO_H

#include <string.h>

#define FWSTARTUPINFO_DEFAULT_NUMRAWSPUS        3
#define FWSTARTUPINFO_DEFAULT_USEDEBUGCONSOLE   true
#define FWSTARTUPINFO_DEFAULT_WINDOWTITLE       "Sample Framework"
#define FWSTARTUPINFO_DEFAULT_ARGUMENTSFILENAME "fwArgs.txt"

class FWStartupInfo {
public:
	FWStartupInfo()
		: mNumRawSPUs(FWSTARTUPINFO_DEFAULT_NUMRAWSPUS),
		  mUseDebugConsole(FWSTARTUPINFO_DEFAULT_USEDEBUGCONSOLE)
	{
		strcpy(mWindowTitle, FWSTARTUPINFO_DEFAULT_WINDOWTITLE);
		strcpy(mArgumentsFilename, FWSTARTUPINFO_DEFAULT_ARGUMENTSFILENAME);
	}

	~FWStartupInfo() {}

	int  mNumRawSPUs;
	bool mUseDebugConsole;
	char mWindowTitle[256];
	char mArgumentsFilename[256];
};

#endif /* PS3TC_FW_STARTUP_INFO_H */
