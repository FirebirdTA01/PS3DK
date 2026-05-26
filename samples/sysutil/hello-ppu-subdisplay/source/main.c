/*
 * hello-ppu-subdisplay - cellSubDisplay surface validation test.
 *
 * Exercises every entry point in libsysutil_subdisplay_stub.a
 * (11 functions).  The API manages a PSP-as-secondary-display link:
 * setup, peer discovery, video buffer access, audio output, and
 * touch input.
 *
 * Runtime expectation: RPCS3 does not HLE cellSubDisplay, so every
 * call after GetRequiredMemory is expected to return an error code.
 * The validation gate is: does the binary link, boot, call all 11 entry
 * points, and reach sys_process_exit(0) without crashing.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/memory.h>

#include <cell/sysutil.h>
#include <sysutil/sysutil_subdisplay.h>

SYS_PROCESS_PARAM(1001, 0x10000);

_Static_assert(CELL_SYSUTIL_ERROR_BASE_SUBDISPLAY == 0x80029800,
               "subdisplay error base must match SDK ABI");
_Static_assert(CELL_SUBDISPLAY_TOUCH_MAX_TOUCH_INFO == 6,
               "touch info array size must match SDK ABI");
_Static_assert(CELL_SUBDISPLAY_0003_WIDTH == 864,
               "subdisplay 0003 width must match SDK ABI");
_Static_assert(CELL_SUBDISPLAY_0003_HEIGHT == 480,
               "subdisplay 0003 height must match SDK ABI");

static volatile int g_cb_count = 0;

static void subdisplay_handler(int cbMsg, uint64_t cbParam, void *userdata)
{
	(void)userdata;
	g_cb_count++;
	printf("[subdisplay] cb msg=%d param=%llu\n", cbMsg,
	       (unsigned long long)cbParam);
}

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	printf("  sysutil event: 0x%08llx\n", (unsigned long long)status);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-subdisplay: cellSubDisplay surface validation\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	printf("  cellSysutilRegisterCallback -> 0x%08x\n", (unsigned)rc);

	/* GetRequiredMemory */
	CellSubDisplayParam param = {
		.version = CELL_SUBDISPLAY_VERSION_0001,
		.mode    = CELL_SUBDISPLAY_MODE_REMOTEPLAY,
		.nGroup  = 1,
		.nPeer   = 1,
		.videoParam = {
			.format      = CELL_SUBDISPLAY_VIDEO_FORMAT_A8R8G8B8,
			.width       = 864,
			.height      = 480,
			.pitch       = 864,
			.aspectRatio = CELL_SUBDISPLAY_VIDEO_ASPECT_RATIO_16_9,
			.videoMode   = CELL_SUBDISPLAY_VIDEO_MODE_SETDATA,
		},
		.audioParam = {
			.ch        = 2,
			.audioMode = CELL_SUBDISPLAY_AUDIO_MODE_SETDATA,
		},
	};

	int memSize = cellSubDisplayGetRequiredMemory(&param);
	printf("  cellSubDisplayGetRequiredMemory -> %d (0x%08x)\n",
	       memSize, (unsigned)memSize);

	/* Init with an invalid container so the call validates linkage
	 * without allocating a real memory container. */
	rc = cellSubDisplayInit(&param, subdisplay_handler, NULL,
	                        (sys_memory_container_t)SYS_MEMORY_CONTAINER_ID_INVALID);
	printf("  cellSubDisplayInit -> 0x%08x\n", (unsigned)rc);

	rc = cellSubDisplayStart();
	printf("  cellSubDisplayStart -> 0x%08x\n", (unsigned)rc);

	/* Pump callbacks briefly */
	for (int i = 0; i < 5; i++) {
		cellSysutilCheckCallback();
		usleep(50 * 1000);
	}

	int peerNum = cellSubDisplayGetPeerNum(0);
	printf("  cellSubDisplayGetPeerNum(0) -> %d (0x%08x)\n",
	       peerNum, (unsigned)peerNum);

	CellSubDisplayPeerInfo peerInfo;
	int peerCount = 1;
	rc = cellSubDisplayGetPeerList(0, &peerInfo, &peerCount);
	printf("  cellSubDisplayGetPeerList(0) -> 0x%08x, count=%d\n",
	       (unsigned)rc, peerCount);

	CellSubDisplayTouchInfo touchInfo[CELL_SUBDISPLAY_TOUCH_MAX_TOUCH_INFO];
	int touchCount = CELL_SUBDISPLAY_TOUCH_MAX_TOUCH_INFO;
	rc = cellSubDisplayGetTouchInfo(0, touchInfo, &touchCount);
	printf("  cellSubDisplayGetTouchInfo(0) -> 0x%08x, count=%d\n",
	       (unsigned)rc, touchCount);

	void *pVideoBuf = NULL;
	uint32_t videoSize = 0;
	rc = cellSubDisplayGetVideoBuffer(0, &pVideoBuf, &videoSize);
	printf("  cellSubDisplayGetVideoBuffer(0) -> 0x%08x, buf=%p, size=%u\n",
	       (unsigned)rc, pVideoBuf, (unsigned)videoSize);

	int16_t audioBuf[480] = {0};
	rc = cellSubDisplayAudioOutBlocking(0, audioBuf, 480);
	printf("  cellSubDisplayAudioOutBlocking(0) -> 0x%08x\n", (unsigned)rc);

	rc = cellSubDisplayAudioOutNonBlocking(0, audioBuf, 480);
	printf("  cellSubDisplayAudioOutNonBlocking(0) -> 0x%08x\n", (unsigned)rc);

	/* Pump callbacks again */
	for (int i = 0; i < 5; i++) {
		cellSysutilCheckCallback();
		usleep(50 * 1000);
	}

	rc = cellSubDisplayStop();
	printf("  cellSubDisplayStop -> 0x%08x\n", (unsigned)rc);

	rc = cellSubDisplayEnd();
	printf("  cellSubDisplayEnd -> 0x%08x\n", (unsigned)rc);

	cellSysutilUnregisterCallback(0);
	printf("  callbacks received: %d\n", g_cb_count);
	printf("result: PASS (validation complete)\n");

	sys_process_exit(0);
	return 0;
}
