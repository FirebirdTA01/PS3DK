/*
 * hello-ppu-avc — exercise the legacy AVC voice-chat panel surface
 * (cellSysutilAvc*).  No real chat session is established; we just
 * Load the panel (no NP room), query its show status, mute toggles,
 * layout-mode, speaker volume, and Unload.
 *
 * Signatures cross-checked against RPCS3's cellSysutilAvc.{h,cpp}.
 * RPCS3 stubs most AVC entry points to no-ops, so this sample is
 * mainly a link-and-resolve smoke test plus return-code logging.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/memory.h>
#include <sys/process.h>
#include <cell/sysutil.h>
#include <cell/sysutil_avc.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define AVC_HEAP_BYTES (4 * 1024 * 1024)

static volatile int g_event_count = 0;
static volatile int g_load_result = -1;
static volatile int g_unload_result = -1;

static void avc_cb(CellSysutilAvcRequestId request_id,
                   CellSysutilAvcEvent event,
                   CellSysutilAvcEventParam param,
                   void *userdata)
{
	(void)userdata;
	g_event_count++;
	const char *name = "?";
	switch (event) {
	case CELL_AVC_EVENT_LOAD_SUCCEEDED:    name = "LOAD_SUCCEEDED";   g_load_result = 0; break;
	case CELL_AVC_EVENT_LOAD_FAILED:       name = "LOAD_FAILED";      g_load_result = 1; break;
	case CELL_AVC_EVENT_UNLOAD_SUCCEEDED:  name = "UNLOAD_SUCCEEDED"; g_unload_result = 0; break;
	case CELL_AVC_EVENT_UNLOAD_FAILED:     name = "UNLOAD_FAILED";    g_unload_result = 1; break;
	case CELL_AVC_EVENT_JOIN_SUCCEEDED:    name = "JOIN_SUCCEEDED";    break;
	case CELL_AVC_EVENT_JOIN_FAILED:       name = "JOIN_FAILED";       break;
	case CELL_AVC_EVENT_BYE_SUCCEEDED:     name = "BYE_SUCCEEDED";     break;
	case CELL_AVC_EVENT_BYE_FAILED:        name = "BYE_FAILED";        break;
	default:                               name = "system event";      break;
	}
	printf("  avc-event: req=0x%08x %s param=0x%08x\n",
	       (unsigned)request_id, name, (unsigned)param);
}

int main(void)
{
	printf("hello-ppu-avc: cellSysutilAvc* exercise (panel only)\n");

	int rc = cellSysutilRegisterCallback(0, NULL, NULL);
	(void)rc;

	sys_memory_container_t mem = 0;
	rc = sysMemContainerCreate(&mem, AVC_HEAP_BYTES);
	printf("  sysMemContainerCreate(%u) -> 0x%08x\n",
	       (unsigned)AVC_HEAP_BYTES, (unsigned)rc);

	CellSysutilAvcRequestId load_req = 0;
	rc = cellSysutilAvcLoadAsync(avc_cb, NULL, mem,
	                             CELL_SYSUTIL_AVC_VOICE_CHAT,
	                             CELL_SYSUTIL_AVC_VIDEO_QUALITY_DEFAULT,
	                             CELL_SYSUTIL_AVC_VOICE_QUALITY_DEFAULT,
	                             &load_req);
	printf("  cellSysutilAvcLoadAsync -> rc=0x%08x req=0x%08x\n",
	       (unsigned)rc, (unsigned)load_req);

	for (int i = 0; i < 30 && g_load_result < 0; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	rc = cellSysutilAvcShowPanel();
	printf("  cellSysutilAvcShowPanel -> 0x%08x\n", (unsigned)rc);

	uint8_t visible = 0;
	rc = cellSysutilAvcGetShowStatus(&visible);
	printf("  cellSysutilAvcGetShowStatus -> rc=0x%08x visible=%u\n",
	       (unsigned)rc, visible);

	CellSysutilAvcLayoutMode mode = (CellSysutilAvcLayoutMode)0;
	rc = cellSysutilAvcGetLayoutMode(&mode);
	printf("  cellSysutilAvcGetLayoutMode -> rc=0x%08x mode=%d\n",
	       (unsigned)rc, (int)mode);
	rc = cellSysutilAvcSetLayoutMode(CELL_SYSUTIL_AVC_LAYOUT_BOTTOM);
	printf("  cellSysutilAvcSetLayoutMode(BOTTOM) -> 0x%08x\n", (unsigned)rc);

	int32_t volume = 0;
	rc = cellSysutilAvcGetSpeakerVolumeLevel(&volume);
	printf("  cellSysutilAvcGetSpeakerVolumeLevel -> rc=0x%08x vol=%d\n",
	       (unsigned)rc, volume);
	rc = cellSysutilAvcSetSpeakerVolumeLevel(8);
	printf("  cellSysutilAvcSetSpeakerVolumeLevel(8) -> 0x%08x\n", (unsigned)rc);

	uint8_t muting = 0;
	rc = cellSysutilAvcGetVoiceMuting(&muting);
	printf("  cellSysutilAvcGetVoiceMuting -> rc=0x%08x muting=%u\n",
	       (unsigned)rc, muting);
	rc = cellSysutilAvcSetVoiceMuting(1);
	printf("  cellSysutilAvcSetVoiceMuting(ON) -> 0x%08x\n", (unsigned)rc);

	rc = cellSysutilAvcGetVideoMuting(&muting);
	printf("  cellSysutilAvcGetVideoMuting -> rc=0x%08x muting=%u\n",
	       (unsigned)rc, muting);
	rc = cellSysutilAvcSetVideoMuting(1);
	printf("  cellSysutilAvcSetVideoMuting(ON) -> 0x%08x\n", (unsigned)rc);

	rc = cellSysutilAvcHidePanel();
	printf("  cellSysutilAvcHidePanel -> 0x%08x\n", (unsigned)rc);

	CellSysutilAvcRequestId unload_req = 0;
	rc = cellSysutilAvcUnloadAsync(&unload_req);
	printf("  cellSysutilAvcUnloadAsync -> rc=0x%08x req=0x%08x\n",
	       (unsigned)rc, (unsigned)unload_req);

	for (int i = 0; i < 30 && g_unload_result < 0; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	cellSysutilUnregisterCallback(0);
	sysMemContainerDestroy(mem);
	printf("  events=%d load=%d unload=%d\n",
	       g_event_count, g_load_result, g_unload_result);
	return 0;
}
