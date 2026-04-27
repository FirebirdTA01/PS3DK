/*
 * hello-ppu-videoout-info — exercise the late-SDK cellVideoOut*
 * surface that resolves through libsysutil_stub: GetNumberOfDevice,
 * GetDeviceInfo, RegisterCallback, UnregisterCallback,
 * GetConvertCursorColorInfo.
 *
 * Expected runtime behaviour on RPCS3:
 *   1. Print number of attached video-out devices (typically 1).
 *   2. Print port type / colour space / latency / RGB output range
 *      / availableModeCount for the primary device.
 *   3. Register a video-out callback and idle a few seconds so any
 *      connect/disconnect/auth events get printed.
 *   4. Unregister the callback and exit cleanly.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <cell/cell_video_out.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile int g_callbacks = 0;

static int videoout_cb(uint32_t slot, uint32_t videoOut, uint32_t deviceIndex,
                       uint32_t event, CellVideoOutDeviceInfo *info,
                       void *userData)
{
	(void)info; (void)userData;
	g_callbacks++;
	const char *ev = "?";
	switch (event) {
	case CELL_VIDEO_OUT_EVENT_DEVICE_CHANGED:       ev = "DEVICE_CHANGED"; break;
	case CELL_VIDEO_OUT_EVENT_OUTPUT_DISABLED:      ev = "OUTPUT_DISABLED"; break;
	case CELL_VIDEO_OUT_EVENT_DEVICE_AUTHENTICATED: ev = "DEVICE_AUTHENTICATED"; break;
	case CELL_VIDEO_OUT_EVENT_OUTPUT_ENABLED:       ev = "OUTPUT_ENABLED"; break;
	}
	printf("  videoOut callback: slot=%u videoOut=%u dev=%u event=%s\n",
	       (unsigned)slot, (unsigned)videoOut, (unsigned)deviceIndex, ev);
	return 0;
}

int main(void)
{
	printf("hello-ppu-videoout-info: cellVideoOut* exploration\n");

	int n = cellVideoOutGetNumberOfDevice(CELL_VIDEO_OUT_PRIMARY);
	printf("  cellVideoOutGetNumberOfDevice -> %d\n", n);

	CellVideoOutDeviceInfo info;
	memset(&info, 0, sizeof(info));
	int rc = cellVideoOutGetDeviceInfo(CELL_VIDEO_OUT_PRIMARY, 0, &info);
	if (rc == 0) {
		printf("  primary device: portType=0x%02x colorSpace=0x%02x "
		       "latency=%u modes=%u state=%u rgbRange=%u\n",
		       info.portType, info.colorSpace,
		       (unsigned)info.latency,
		       (unsigned)info.availableModeCount,
		       (unsigned)info.state,
		       (unsigned)info.rgbOutputRange);
	} else {
		printf("  cellVideoOutGetDeviceInfo -> 0x%08x\n", (unsigned)rc);
	}

	uint8_t cursorRange = 0xff;
	rc = cellVideoOutGetConvertCursorColorInfo(&cursorRange);
	printf("  cellVideoOutGetConvertCursorColorInfo -> rc=0x%08x range=%u\n",
	       (unsigned)rc, (unsigned)cursorRange);

	rc = cellVideoOutRegisterCallback(0, videoout_cb, NULL);
	printf("  cellVideoOutRegisterCallback(slot=0) -> 0x%08x\n", (unsigned)rc);

	/* idle 3 seconds so any spontaneous events surface */
	for (int i = 0; i < 30; i++)
		usleep(100 * 1000);

	rc = cellVideoOutUnregisterCallback(0);
	printf("  cellVideoOutUnregisterCallback -> 0x%08x\n", (unsigned)rc);
	printf("  callbacks observed: %d\n", g_callbacks);
	return 0;
}
