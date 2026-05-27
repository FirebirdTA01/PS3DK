/*
 * PS3 Custom Toolchain - samples/fw/include/FWApplication.h
 *
 * Base application lifecycle interface for the sample fw layer.
 */

#ifndef PS3TC_FW_APPLICATION_H
#define PS3TC_FW_APPLICATION_H

#include <FWDebug.h>
#include <FWDisplayInfo.h>
#include <FWStartupInfo.h>

class FWApplication {
public:
	FWApplication();
	virtual ~FWApplication();

	virtual bool onInit(int argc, char **ppArgv);
	virtual void onShutdown();
	virtual bool onUpdate();
	virtual void onRender() = 0;
	virtual void onSize(const FWDisplayInfo &dispInfo) = 0;

	FWDisplayInfo &getDisplayInfo() { return mDispInfo; }
	const FWDisplayInfo &getDisplayInfo() const { return mDispInfo; }
	FWStartupInfo &getStartupInfo() { return mStartupInfo; }
	const FWStartupInfo &getStartupInfo() const { return mStartupInfo; }

	static FWApplication *getApplication()
	{
		FWASSERT(spApplication != 0);
		return spApplication;
	}

protected:
	FWDisplayInfo mDispInfo;
	FWStartupInfo mStartupInfo;

private:
	static FWApplication *spApplication;
};

#endif /* PS3TC_FW_APPLICATION_H */
