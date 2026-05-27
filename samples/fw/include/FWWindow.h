/*
 * PS3 Custom Toolchain - samples/fw/include/FWWindow.h
 *
 * Base window lifecycle interface for the sample fw layer.
 */

#ifndef PS3TC_FW_WINDOW_H
#define PS3TC_FW_WINDOW_H

#include <FWApplication.h>
#include <FWDebugConsole.h>
#include <FWDebugFont.h>
#include <FWDisplayInfo.h>
#include <FWStartupInfo.h>

class FWWindow {
public:
	FWWindow(int argc, char **ppArgv, const FWDisplayInfo &dispInfo, const FWStartupInfo &startupInfo);
	virtual ~FWWindow();

	void init();
	void update();
	void render();
	void destroy();
	void parseArgumentsFile(const char *pArgumentsFile);
	void resize(int width, int height);

	static FWWindow *getWindow()
	{
		FWASSERT(spWindow != 0);
		return spWindow;
	}

	virtual void *getWindowSystemContext() = 0;
	virtual FWDisplayInfo &getDisplayInfo() = 0;
	virtual void setRenderingContext() = 0;
	virtual void clearRenderingContext() = 0;
	virtual void flip() = 0;

protected:
	bool mUseDebugConsole;
	bool mInit;
	bool mUpdate;
	int mArgc;
	char **mppArgv;
	char *mpArgFile;

private:
	static FWWindow *spWindow;
};

#endif /* PS3TC_FW_WINDOW_H */
