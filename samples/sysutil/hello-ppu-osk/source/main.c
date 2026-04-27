/*
 * hello-ppu-osk — bring up the on-screen keyboard and exercise the
 * late-SDK extension entry points (SetLayoutMode,
 * SetInitialKeyLayout, SetInitialInputDevice, AddSupportLanguage,
 * SetKeyLayoutOption, DisableDimmer, GetSize, SetDeviceMask) before
 * LoadAsync, then UnloadAsync once the user finishes.
 *
 * Expected runtime behaviour on RPCS3:
 *   1. OSK panel appears with the configured initial layout.
 *   2. User types something and confirms (or cancels).
 *   3. Sysutil callback fires: LOADED -> INPUT_ENTERED -> FINISHED ->
 *      UNLOADED.  We unload-async on FINISHED and copy the result
 *      string out as UTF-16 -> ASCII (best-effort).
 *
 * If you'd rather just verify the OSK is wired up at all, this sample
 * times out after ~30s and prints whichever events were observed.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/memory.h>
#include <sys/process.h>
#include <cell/sysutil.h>
#include <cell/sysutil_oskdialog.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define OSK_HEAP_BYTES (2 * 1024 * 1024)

static volatile int g_finished = 0;
static volatile int g_unloaded = 0;
static volatile int g_input_entered = 0;
static uint16_t g_result_buf[CELL_OSKDIALOG_STRING_SIZE];

static const uint16_t k_message[] = { 'T','y','p','e',' ','s','o','m','e','t','h','i','n','g',':',0 };
static const uint16_t k_init_text[] = { 'h','e','l','l','o',0 };

static void osk_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param; (void)userdata;
	switch (status) {
	case CELL_SYSUTIL_OSKDIALOG_LOADED:
		printf("  osk: LOADED\n");
		break;
	case CELL_SYSUTIL_OSKDIALOG_INPUT_ENTERED:
		printf("  osk: INPUT_ENTERED\n");
		g_input_entered = 1;
		break;
	case CELL_SYSUTIL_OSKDIALOG_INPUT_CANCELED:
		printf("  osk: INPUT_CANCELED\n");
		break;
	case CELL_SYSUTIL_OSKDIALOG_FINISHED: {
		printf("  osk: FINISHED — calling UnloadAsync\n");
		CellOskDialogCallbackReturnParam ret;
		memset(&ret, 0, sizeof(ret));
		ret.pResultString = g_result_buf;
		int rc = cellOskDialogUnloadAsync(&ret);
		printf("  cellOskDialogUnloadAsync -> 0x%08x (chars=%d)\n",
		       (unsigned)rc, ret.numCharsResultString);
		g_finished = 1;
		break;
	}
	case CELL_SYSUTIL_OSKDIALOG_UNLOADED:
		printf("  osk: UNLOADED\n");
		g_unloaded = 1;
		break;
	case CELL_SYSUTIL_OSKDIALOG_DISPLAY_CHANGED:
		printf("  osk: DISPLAY_CHANGED\n");
		break;
	}
}

int main(void)
{
	printf("hello-ppu-osk: cellOskDialog* with extension entry points\n");

	int rc = cellSysutilRegisterCallback(0, osk_event, NULL);
	if (rc != 0) {
		printf("  cellSysutilRegisterCallback -> 0x%08x — abort\n", (unsigned)rc);
		return 1;
	}

	/* extension entry points before LoadAsync */
	rc = cellOskDialogSetLayoutMode(CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_CENTER |
	                                CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_CENTER);
	printf("  cellOskDialogSetLayoutMode -> 0x%08x\n", (unsigned)rc);

	rc = cellOskDialogSetInitialKeyLayout(CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_FULLKEY);
	printf("  cellOskDialogSetInitialKeyLayout(FULLKEY) -> 0x%08x\n", (unsigned)rc);

	rc = cellOskDialogSetInitialInputDevice(CELL_OSKDIALOG_INPUT_DEVICE_PAD);
	printf("  cellOskDialogSetInitialInputDevice(PAD) -> 0x%08x\n", (unsigned)rc);

	rc = cellOskDialogAddSupportLanguage(CELL_OSKDIALOG_PANELMODE_ENGLISH |
	                                     CELL_OSKDIALOG_PANELMODE_NUMERAL);
	printf("  cellOskDialogAddSupportLanguage(ENGLISH|NUMERAL) -> 0x%08x\n",
	       (unsigned)rc);

	rc = cellOskDialogSetKeyLayoutOption(0);
	printf("  cellOskDialogSetKeyLayoutOption(0) -> 0x%08x\n", (unsigned)rc);

	rc = cellOskDialogDisableDimmer();
	printf("  cellOskDialogDisableDimmer -> 0x%08x\n", (unsigned)rc);

	rc = cellOskDialogSetDeviceMask(CELL_OSKDIALOG_DEVICE_MASK_PAD);
	printf("  cellOskDialogSetDeviceMask(PAD) -> 0x%08x\n", (unsigned)rc);

	uint16_t w = 0, h = 0;
	rc = cellOskDialogGetSize(&w, &h, CELL_OSKDIALOG_TYPE_SINGLELINE_OSK);
	printf("  cellOskDialogGetSize(SINGLELINE) -> rc=0x%08x size=%ux%u\n",
	       (unsigned)rc, w, h);

	/* allocate a memory container for the OSK */
	sys_memory_container_t mem = 0;
	rc = sysMemContainerCreate(&mem, OSK_HEAP_BYTES);
	if (rc != 0) {
		printf("  sysMemContainerCreate -> 0x%08x — abort\n", (unsigned)rc);
		cellSysutilUnregisterCallback(0);
		return 1;
	}

	CellOskDialogParam p;
	memset(&p, 0, sizeof(p));
	p.allowOskPanelFlg  = CELL_OSKDIALOG_PANELMODE_ENGLISH;
	p.firstViewPanel    = CELL_OSKDIALOG_PANELMODE_ENGLISH;
	p.controlPoint.x    = 0.0f;
	p.controlPoint.y    = 0.0f;
	p.prohibitFlgs      = CELL_OSKDIALOG_NO_RETURN;

	CellOskDialogInputFieldInfo ifi;
	memset(&ifi, 0, sizeof(ifi));
	ifi.message       = (uint16_t *)k_message;
	ifi.init_text     = (uint16_t *)k_init_text;
	ifi.limit_length  = 32;

	rc = cellOskDialogLoadAsync(mem, &p, &ifi);
	printf("  cellOskDialogLoadAsync -> 0x%08x\n", (unsigned)rc);
	if (rc != 0) {
		sysMemContainerDestroy(mem);
		cellSysutilUnregisterCallback(0);
		return 1;
	}

	/* drive callbacks for up to ~30s */
	for (int i = 0; i < 300 && !g_unloaded; i++) {
		cellSysutilCheckCallback();
		usleep(100 * 1000);
	}

	if (g_input_entered) {
		printf("  result chars present (%d) — content not displayed (UTF-16)\n",
		       (int)g_result_buf[0]);
	}

	cellSysutilUnregisterCallback(0);
	sysMemContainerDestroy(mem);
	printf("  finished=%d unloaded=%d input_entered=%d\n",
	       g_finished, g_unloaded, g_input_entered);
	return 0;
}
