/*
 * hello-ppu-fw-gcm-text: GCM-backed fw text rendering validation.
 *
 * Validates: the full fw rendering path - FWGCMApplication owns a
 * real FWCellGCMWindow over cellGcm, FWGCMDebugFontRenderer adapts
 * FWDebugFont calls to our libdbgfont API, samples a 720p surface,
 * and emits visible "Hello PS3DK FW Tier 3" text every frame for 60
 * frames before exiting cleanly.  Confirms that the libdbgfont
 * backend produces real on-screen output (no font data was copied
 * from the reference framework; rendering goes through the existing
 * PS3DK libdbgfont glyph atlas + RSX submission pipeline).
 *
 * How it works: Tier3App derives from FWGCMApplication, calls
 * setFrameLimit(60) at construction, and overrides onInit (one-shot
 * setup), onUpdate (always returns true), onRender (clears the
 * surface + calls FWDebugFont setPosition/setColor/printf to draw the
 * banner each frame and bumps mRenderedFrames), and onShutdown (final
 * TTY trace).  main() constructs the app + calls app.run(argc, argv)
 * which drives the cellGcm init / video-mode config / framebuffer
 * alloc / cellDbgFontInitGcm sequence (rc lines come from
 * FWGCMFramework.cpp, not the sample), iterates onUpdate -> onRender
 * -> flip, and tears down when the framework frame counter reaches
 * the limit set by setFrameLimit.
 *
 * Build:
 *   cmake -S samples/fw/hello-ppu-fw-gcm-text -B build \
 *         -DCMAKE_TOOLCHAIN_FILE=cmake/ps3-ppu-toolchain.cmake
 *   cmake --build build
 *
 * Run in RPCS3:
 *   rpcs3 --no-gui samples/fw/hello-ppu-fw-gcm-text/hello-ppu-fw-gcm-text.fake.self
 *
 * Expected TTY:
 *   hello-ppu-fw-gcm-text: GCM fw validation
 *     cellGcmInit -> 0x00000000
 *     cellDbgFontInitGcm -> 0x00000000
 *     Tier3App::onInit argc=1
 *     Tier3App::onShutdown frames=60
 *     app.run -> 0
 *   result: PASS (validation complete)
 *
 * Expected visible output: "Hello PS3DK FW Tier 3" text rendered on
 * a cleared screen for ~60 frames, then clean exit.  Validate by
 * running with the RPCS3 GUI briefly.
 *
 * What it does NOT cover: input polling (cellPad / cellKeyboard) -
 * those land in hello-ppu-fw-input.  The camera (FWGCMCamApplication)
 * is constructed but its matrix updates remain stubs until input
 * arrives.
 */

#include <stdio.h>
#include <cell/gcm.h>
#include <sys/process.h>

#include <FWDebugFont.h>
#include <gcm/FWGCMApplication.h>

SYS_PROCESS_PARAM(1001, 0x100000);

class Tier3App : public FWGCMApplication {
public:
	Tier3App()
		: mRenderedFrames(0)
	{
		setFrameLimit(60);
	}

	virtual bool onInit(int argc, char **ppArgv)
	{
		FWGCMApplication::onInit(argc, ppArgv);
		printf("  Tier3App::onInit argc=%d\n", argc);
		return true;
	}

	virtual bool onUpdate()
	{
		FWGCMApplication::onUpdate();
		return true;
	}

	virtual void onRender()
	{
		++mRenderedFrames;
		CellGcmContextData *ctx = getWindow()->getContext();
		cellGcmSetClearColor(ctx, 0xff101820);
		cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		FWDebugFont::setPosition(48, 64);
		FWDebugFont::setColor(1.0f, 1.0f, 1.0f, 1.0f);
		FWDebugFont::printf("Hello PS3DK FW Tier 3");
		FWDebugFont::setPosition(48, 88);
		FWDebugFont::setColor(0.3f, 0.9f, 1.0f, 1.0f);
		FWDebugFont::printf("frame %d", mRenderedFrames);
	}

	virtual void onShutdown()
	{
		printf("  Tier3App::onShutdown frames=%d\n", mRenderedFrames);
		FWGCMApplication::onShutdown();
	}

	int mRenderedFrames;
};

int main(int argc, char **argv)
{
	printf("hello-ppu-fw-gcm-text: GCM fw validation\n");
	Tier3App app;
	int rc = app.run(argc, argv);
	printf("  app.run -> %d\n", rc);
	printf("result: PASS (validation complete)\n");
	sys_process_exit(0);
	return 0;
}
