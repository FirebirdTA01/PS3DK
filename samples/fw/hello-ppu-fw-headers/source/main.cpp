/*
 * hello-ppu-fw-headers: header-only fw compatibility surface validation.
 *
 * Validates: the macro / config / time slice of the sample framework
 * compatibility layer in isolation - no lifecycle, no renderer, no
 * GCM context.  Pulls in FWAssert / FWDebug / FWDisplayInfo / FWMath /
 * FWStartupInfo / FWTime / gcm/FWGCMConfiguration and constructs each
 * config class with its default values.
 *
 * How it works: prints a banner, constructs FWStartupInfo +
 * FWDisplayInfo + FWGCMConfiguration on the stack and dumps their
 * default fields to TTY, exercises Vectormath::Aos Point3 / Vector3 /
 * Matrix4 paths via FWMath, runs a small FWTime::init -> update ->
 * FWTimeVal arithmetic exchange, then exits cleanly via
 * sys_process_exit(0).  Header-only chain: nothing from samples/fw/src
 * is linked in.
 *
 * Build:
 *   cmake -S samples/fw/hello-ppu-fw-headers -B build \
 *         -DCMAKE_TOOLCHAIN_FILE=cmake/ps3-ppu-toolchain.cmake
 *   cmake --build build
 *
 * Run in RPCS3:
 *   rpcs3 --no-gui samples/fw/hello-ppu-fw-headers/hello-ppu-fw-headers.fake.self
 *
 * Expected TTY:
 *   hello-ppu-fw-headers: fw header validation
 *     startup raw_spus=3 debug_console=1 title=Sample Framework
 *     display 1280x720 color=32 depth=24
 *     gcm cb=524288 state=524288 color=0x00000008
 *     math point=(1.0, 2.0, 3.0) vector_x=1.0 projected_w=1.0
 *     time elapsed=1.500
 *   result: PASS (validation complete)
 *
 * What it does NOT cover: the FWApplication / FWWindow lifecycle path,
 * the FWDebugFontRenderer surface, or any actual rendering.  See
 * hello-ppu-fw-null-lifecycle (Null-renderer lifecycle exercise) and
 * hello-ppu-fw-gcm-text (real GCM-backed text rendering) for those.
 */

#include <stdio.h>
#include <sys/process.h>

#include <FWAssert.h>
#include <FWDebug.h>
#include <FWDisplayInfo.h>
#include <FWMath.h>
#include <FWStartupInfo.h>
#include <FWTime.h>
#include <gcm/FWGCMConfiguration.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	printf("hello-ppu-fw-headers: fw header validation\n");

	FWASSERT(true);
	FWWARNING("fw warning path");

	FWStartupInfo startup;
	FWDisplayInfo display;
	FWGCMConfiguration gcm;

	Point3 point(1.0f, 2.0f, 3.0f);
	Vector3 vector(1.0f, 0.0f, 0.0f);
	Matrix4 matrix = Matrix4::identity();
	Vector4 projected = matrix * Vector4(point);

	FWTime::init();
	FWTime::update();
	FWTimeVal elapsed = FWTimeVal(1.0) + FWTimeVal(0.5);
	double seconds = (double)elapsed;

	printf("  startup raw_spus=%d debug_console=%d title=%s\n",
	       startup.mNumRawSPUs,
	       startup.mUseDebugConsole ? 1 : 0,
	       startup.mWindowTitle);
	printf("  display %dx%d color=%d depth=%d\n",
	       display.mWidth,
	       display.mHeight,
	       display.mColorBits,
	       display.mDepthBits);
	printf("  gcm cb=%u state=%u color=0x%08x\n",
	       (unsigned)gcm.mDefaultCBSize,
	       (unsigned)gcm.mDefaultStateCmdSize,
	       (unsigned)gcm.mColorFormat);
	printf("  math point=(%.1f, %.1f, %.1f) vector_x=%.1f projected_w=%.1f\n",
	       (double)(float)point.getX(),
	       (double)(float)point.getY(),
	       (double)(float)point.getZ(),
	       (double)(float)vector.getX(),
	       (double)(float)projected.getW());
	printf("  time elapsed=%.3f\n", seconds);
	printf("result: PASS (validation complete)\n");

	sys_process_exit(0);
	return 0;
}
