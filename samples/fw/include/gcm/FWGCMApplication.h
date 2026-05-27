/*
 * PS3 Custom Toolchain - samples/fw/include/gcm/FWGCMApplication.h
 *
 * GCM-backed application lifecycle for fw samples.
 */

#ifndef PS3TC_FW_GCM_APPLICATION_H
#define PS3TC_FW_GCM_APPLICATION_H

#include <FWApplication.h>
#include <gcm/FWCellGCMWindow.h>
#include <gcm/FWGCMConfiguration.h>
#include <gcm/FWGCMDebugFontRenderer.h>

class FWGCMApplication : public FWApplication {
public:
	FWGCMApplication();
	virtual ~FWGCMApplication();

	virtual bool onInit(int argc, char **ppArgv);
	virtual void onShutdown();
	virtual bool onUpdate();
	virtual void onRender();
	virtual void onSize(const FWDisplayInfo &dispInfo);

	int run(int argc, char **ppArgv);
	void requestExit();
	void setFrameLimit(int frameLimit);

	FWGCMConfiguration &getGCMConfiguration() { return mConfiguration; }
	FWCellGCMWindow *getWindow() const { return mWindow; }
	static FWGCMApplication *getGCMApplication();

private:
	static FWGCMApplication *spGCMApplication;

protected:
	FWGCMConfiguration mConfiguration;
	FWCellGCMWindow *mWindow;
	FWGCMDebugFontRenderer mDebugFontRenderer;
	bool mExitRequested;
	int mFrameLimit;
	int mFrameCount;
};

#endif /* PS3TC_FW_GCM_APPLICATION_H */
