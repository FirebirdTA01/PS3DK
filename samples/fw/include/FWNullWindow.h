/*
 * PS3 Custom Toolchain - samples/fw/include/FWNullWindow.h
 *
 * No-op window implementation for fw header validation.
 */

#ifndef PS3TC_FW_NULL_WINDOW_H
#define PS3TC_FW_NULL_WINDOW_H

#include <FWWindow.h>

class FWNullWindow : public FWWindow {
public:
	FWNullWindow(int argc, char **ppArgv, const FWDisplayInfo &dispInfo, const FWStartupInfo &startupInfo);
	virtual ~FWNullWindow();

	bool update();
	void destroy();

	virtual void *getWindowSystemContext();
	virtual FWDisplayInfo &getDisplayInfo();
	virtual void setRenderingContext();
	virtual void clearRenderingContext();
	virtual void flip();

private:
	FWDisplayInfo mDispInfo;
};

#endif /* PS3TC_FW_NULL_WINDOW_H */
