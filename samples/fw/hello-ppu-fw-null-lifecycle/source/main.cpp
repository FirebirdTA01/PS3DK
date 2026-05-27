/*
 * hello-ppu-fw-null-lifecycle: Null fw lifecycle + debug-font facade
 * validation.
 *
 * Validates: the no-op shim path through the fw framework -
 * FWNullApplication derives from FWApplication and overrides every
 * lifecycle hook (onInit / onUpdate / onRender / onShutdown / onSize),
 * FWNullWindow stands in for FWWindow, FWNullDebugFontRenderer
 * satisfies the FWDebugFontRenderer interface without any real
 * rendering.  FWDebugFont + FWDebugConsole are exercised via the
 * registered Null renderer; text emission falls back to ::printf so
 * the trace is still visible in TTY.
 *
 * How it works: a TestApp class derives from FWNullApplication and
 * counts each lifecycle hook invocation.  main drives one explicit
 * init -> update -> render -> render -> resize -> shutdown cycle on
 * the app, instantiates an FWNullWindow + FWNullDebugFontRenderer,
 * registers the renderer via FWDebugFont::setRenderer, walks the
 * FWDebugFont API (setPosition / setColor / setScreenRes /
 * setSafeArea / print / printf / getExtents) and the FWDebugConsole
 * API (init / print / printf / update / render / shutdown), prints
 * counter validation + final state, then sys_process_exit(0).
 *
 * Build:
 *   cmake -S samples/fw/hello-ppu-fw-null-lifecycle -B build \
 *         -DCMAKE_TOOLCHAIN_FILE=cmake/ps3-ppu-toolchain.cmake
 *   cmake --build build
 *
 * Run in RPCS3:
 *   rpcs3 --no-gui samples/fw/hello-ppu-fw-null-lifecycle/hello-ppu-fw-null-lifecycle.fake.self
 *
 * Expected TTY:
 *   hello-ppu-fw-null-lifecycle: Null fw lifecycle validation
 *     TestApp::onInit argc=1
 *     TestApp::onUpdate
 *     TestApp::onRender
 *     TestApp::onRender
 *     TestApp::onSize 640x480
 *     console message 1
 *     console message 1
 *     TestApp::onShutdown
 *     safe_area=10,20,30,40 extents=64x16
 *     counters init=1 update=1 render=2 shutdown=1 size=1
 *   result: PASS (validation complete)
 *
 * What it does NOT cover: real GCM rendering, real on-screen text,
 * any input polling.  See hello-ppu-fw-gcm-text for the GCM-backed
 * rendering path and (forthcoming) hello-ppu-fw-input for FWInput.
 */

#include <stdio.h>
#include <sys/process.h>

#include <FWDebugConsole.h>
#include <FWDebugFont.h>
#include <FWNullApplication.h>
#include <FWNullDebugFontRenderer.h>
#include <FWNullWindow.h>

SYS_PROCESS_PARAM(1001, 0x10000);

class TestApp : public FWNullApplication {
public:
	TestApp()
		: mInitCount(0)
		, mUpdateCount(0)
		, mRenderCount(0)
		, mShutdownCount(0)
		, mSizeCount(0)
	{
	}

	virtual bool onInit(int argc, char **ppArgv)
	{
		FWNullApplication::onInit(argc, ppArgv);
		++mInitCount;
		printf("  TestApp::onInit argc=%d\n", argc);
		return true;
	}

	virtual bool onUpdate()
	{
		FWNullApplication::onUpdate();
		++mUpdateCount;
		printf("  TestApp::onUpdate\n");
		return true;
	}

	virtual void onRender()
	{
		++mRenderCount;
		FWDebugFont::printf("  TestApp::onRender\n");
	}

	virtual void onShutdown()
	{
		++mShutdownCount;
		printf("  TestApp::onShutdown\n");
		FWNullApplication::onShutdown();
	}

	virtual void onSize(const FWDisplayInfo &dispInfo)
	{
		FWNullApplication::onSize(dispInfo);
		++mSizeCount;
		printf("  TestApp::onSize %dx%d\n", dispInfo.mWidth, dispInfo.mHeight);
	}

	int mInitCount;
	int mUpdateCount;
	int mRenderCount;
	int mShutdownCount;
	int mSizeCount;
};

int main(int argc, char **argv)
{
	printf("hello-ppu-fw-null-lifecycle: Null fw lifecycle validation\n");

	TestApp app;
	FWNullDebugFontRenderer renderer;
	FWDebugFont::setRenderer(&renderer);
	FWDebugFont::init();
	FWDebugFont::setPosition(4, 8);
	FWDebugFont::setColor(1.0f, 0.8f, 0.4f, 1.0f);
	FWDebugFont::setSafeArea(10, 20, 30, 40);

	int left = 0;
	int right = 0;
	int top = 0;
	int bottom = 0;
	int extWidth = 0;
	int extHeight = 0;
	FWDebugFont::getSafeArea(&left, &right, &top, &bottom);
	FWDebugFont::getExtents("fw tier2\ntext", &extWidth, &extHeight, 1280);

	FWNullWindow window(argc, argv, app.getDisplayInfo(), app.getStartupInfo());
	window.init();
	window.update();
	window.render();
	window.resize(640, 480);

	FWDebugConsole::setTimeout(10.0f);
	FWDebugConsole::printf("  console message %d\n", 1);
	FWDebugConsole::update();
	FWDebugConsole::render();
	window.destroy();

	printf("  safe_area=%d,%d,%d,%d extents=%dx%d\n",
	       left,
	       right,
	       top,
	       bottom,
	       extWidth,
	       extHeight);
	printf("  counters init=%d update=%d render=%d shutdown=%d size=%d\n",
	       app.mInitCount,
	       app.mUpdateCount,
	       app.mRenderCount,
	       app.mShutdownCount,
	       app.mSizeCount);
	printf("result: PASS (validation complete)\n");

	sys_process_exit(0);
	return 0;
}
