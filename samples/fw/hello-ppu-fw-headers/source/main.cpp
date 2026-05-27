/*
 * hello-ppu-fw-headers - validate header-only fw compatibility surface.
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
