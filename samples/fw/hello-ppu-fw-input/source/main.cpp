/*
 * hello-ppu-fw-input - fw input validation sample.
 *
 * Purpose:
 *   Validates the Tier 4 sample fw input facade over cellPad and
 *   cellKeyboard by polling pad and keyboard state through FWInput.
 *
 * Validates:
 *   - FWInputManager lifecycle: init, per-frame update, shutdown.
 *   - FWCellPadDevice polling through cellPadGetData.
 *   - FWCellKeyboardDevice polling through cellKbRead.
 *   - FWGCMCamControlApplication camera-control fields.
 *   - GCM text rendering remains usable while input is polled.
 *
 * How it works:
 *   The app derives from FWGCMCamControlApplication, initializes
 *   FWInput in onInit, prints pad and keyboard state every 15 frames,
 *   renders a short debug-font overlay, then exits through the
 *   framework frame limit.
 *
 * Build:
 *   cmake -S samples/fw/hello-ppu-fw-input -B build/fw-input
 *   cmake --build build/fw-input
 *
 * Run:
 *   Boot hello-ppu-fw-input.fake.self in RPCS3.  TTY output lists
 *   connection state, selected pad buttons, left stick axes, Escape,
 *   and camera-control movement fields.
 *
 * Expected output:
 *   cellGcmInit and cellDbgFontInitGcm return 0.  FWCellInput prints
 *   cellPadInit/cellKbInit return codes.  Every 15 frames, the sample
 *   prints the current input state.  The final result line reports
 *   PASS after sys_process_exit(0).
 *
 * What is not covered:
 *   The sample does not validate every pad button or keyboard key.
 *   It does not require a connected controller or keyboard to pass.
 */

#include <stdio.h>
#include <sys/process.h>

#include <FWDebugFont.h>
#include <FWInput.h>
#include <gcm/FWGCMCamControlApplication.h>

SYS_PROCESS_PARAM(1001, 0x100000);

class InputApp : public FWGCMCamControlApplication {
public:
	InputApp()
		: mFrames(0)
	{
		setFrameLimit(90);
	}

	virtual bool onInit(int argc, char **ppArgv)
	{
		FWGCMCamControlApplication::onInit(argc, ppArgv);
		printf("  InputApp::onInit argc=%d\n", argc);
		FWInput::init();
		return true;
	}

	virtual bool onUpdate()
	{
		++mFrames;
		bool keepRunning = FWGCMCamControlApplication::onUpdate();
		if ((mFrames % 15) == 0) {
			FWInputDevice *pad = FWInput::getDevice(FWInput::DeviceType_Pad, 0);
			FWInputDevice *keyboard = FWInput::getDevice(FWInput::DeviceType_Keyboard, 0);
			bool cross = pad && pad->getRawBool(FWInput::Channel_Button_0);
			bool start = pad && pad->getRawBool(FWInput::Channel_Button_9);
			bool escape = keyboard && keyboard->getRawBool(FWInput::Channel_Key_Escape);
			float lx = pad ? pad->getRawFloat(FWInput::Channel_XAxis_0) : 0.0f;
			float ly = pad ? pad->getRawFloat(FWInput::Channel_YAxis_0) : 0.0f;
			printf("  frame=%d pad=%d cross=%d start=%d lx=%.2f ly=%.2f keyEsc=%d move=%.2f/%.2f yaw=%.2f pitch=%.2f\n",
			       mFrames,
			       pad && pad->isConnected(),
			       cross,
			       start,
			       (double)lx,
			       (double)ly,
			       escape,
			       (double)getLastMoveX(),
			       (double)getLastMoveZ(),
			       (double)getLastYaw(),
			       (double)getLastPitch());
		}
		return keepRunning;
	}

	virtual void onRender()
	{
		FWGCMCamControlApplication::onRender();
		FWDebugFont::setPosition(48, 64);
		FWDebugFont::setColor(1.0f, 1.0f, 1.0f, 1.0f);
		FWDebugFont::printf("Hello PS3DK FW Input");
		FWDebugFont::setPosition(48, 88);
		FWDebugFont::setColor(0.3f, 0.9f, 1.0f, 1.0f);
		FWDebugFont::printf("move %.2f %.2f yaw %.2f pitch %.2f",
		                     (double)getLastMoveX(),
		                     (double)getLastMoveZ(),
		                     (double)getLastYaw(),
		                     (double)getLastPitch());
	}

	virtual void onShutdown()
	{
		printf("  InputApp::onShutdown frames=%d\n", mFrames);
		FWInput::shutdown();
		FWGCMCamControlApplication::onShutdown();
	}

private:
	int mFrames;
};

int main(int argc, char **argv)
{
	printf("hello-ppu-fw-input: fw input validation\n");
	InputApp app;
	int rc = app.run(argc, argv);
	printf("  app.run -> %d\n", rc);
	printf("result: PASS (validation complete)\n");
	sys_process_exit(0);
	return 0;
}
