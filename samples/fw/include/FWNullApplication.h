/*
 * PS3 Custom Toolchain - samples/fw/include/FWNullApplication.h
 *
 * No-op application implementation for fw header validation.
 */

#ifndef PS3TC_FW_NULL_APPLICATION_H
#define PS3TC_FW_NULL_APPLICATION_H

#include <FWApplication.h>
#include <FWNullDebugFontRenderer.h>

class FWNullApplication : public FWApplication {
public:
	FWNullApplication();
	virtual ~FWNullApplication();

	virtual bool onInit(int argc, char **ppArgv);
	virtual void onShutdown();
	virtual bool onUpdate();
	virtual void onRender();
	virtual void onSize(const FWDisplayInfo &dispInfo);

private:
	FWNullDebugFontRenderer mFontRenderer;
};

#endif /* PS3TC_FW_NULL_APPLICATION_H */
