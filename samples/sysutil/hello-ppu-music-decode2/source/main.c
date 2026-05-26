/*
 * hello-ppu-music-decode2 - cellMusicDecodeUtility v2 surface validation.
 *
 * Exercises all 10 v2 entry points in libsysutil_music_decode_stub.a.
 * The sample links, calls every stub, prints each return code, and
 * reaches sys_process_exit(0) regardless of runtime results.
 *
 * Runtime expectation: RPCS3 may or may not HLE music decode; return
 * codes will be errors or OK depending on firmware state.  The
 * validation gate is binary boot + all-10-called + clean exit.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/memory.h>

#include <cell/sysutil.h>
#include <sysutil/sysutil_music_decode2.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile int g_cb_count = 0;

static void music_decode2_callback(uint32_t event, void *param, void *userData)
{
	(void)param;
	(void)userData;
	g_cb_count++;
	printf("  [decode2] cb event=%u\n", (unsigned)event);
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

	printf("hello-ppu-music-decode2: cellMusicDecode2 surface validation\n");

	int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
	printf("  cellSysutilRegisterCallback -> 0x%08x\n", (unsigned)rc);

	/* Initialize2: INVALID container, speed 0, max speed, default buf */
	rc = cellMusicDecodeInitialize2(CELL_MUSIC_DECODE2_MODE_NORMAL,
	                                (sys_memory_container_t)SYS_MEMORY_CONTAINER_ID_INVALID,
	                                0, music_decode2_callback, NULL,
	                                CELL_MUSIC_DECODE2_SPEED_MAX,
	                                CELL_MUSIC_DECODE2_MIN_BUFFER_SIZE);
	printf("  cellMusicDecodeInitialize2 -> 0x%08x\n", (unsigned)rc);

	/* Pump callbacks so Initialize2 result can fire */
	for (int i = 0; i < 10; i++) {
		cellSysutilCheckCallback();
		usleep(50 * 1000);
	}

	/* Initialize2SystemWorkload: same INVALID container pattern */
	uint8_t priority[8] = {1,1,1,1,1,1,1,1};
	rc = cellMusicDecodeInitialize2SystemWorkload(
		CELL_MUSIC_DECODE2_MODE_NORMAL,
		(sys_memory_container_t)SYS_MEMORY_CONTAINER_ID_INVALID,
		music_decode2_callback, NULL,
		100, CELL_MUSIC_DECODE2_MIN_BUFFER_SIZE,
		NULL, priority, NULL);
	printf("  cellMusicDecodeInitialize2SystemWorkload -> 0x%08x\n", (unsigned)rc);

	for (int i = 0; i < 10; i++) {
		cellSysutilCheckCallback();
		usleep(50 * 1000);
	}

	/* SelectContents2 */
	rc = cellMusicDecodeSelectContents2();
	printf("  cellMusicDecodeSelectContents2 -> 0x%08x\n", (unsigned)rc);

	/* SetSelectionContext2 + GetSelectionContext2 */
	rc = cellMusicDecodeSetSelectionContext2(NULL);
	printf("  cellMusicDecodeSetSelectionContext2 -> 0x%08x\n", (unsigned)rc);

	CellMusicSelectionContext *ctx = NULL;
	rc = cellMusicDecodeGetSelectionContext2(ctx);
	printf("  cellMusicDecodeGetSelectionContext2 -> 0x%08x\n", (unsigned)rc);

	/* SetDecodeCommand2 */
	rc = cellMusicDecodeSetDecodeCommand2(CELL_MUSIC_DECODE2_CMD_START);
	printf("  cellMusicDecodeSetDecodeCommand2 -> 0x%08x\n", (unsigned)rc);

	/* GetDecodeStatus2 */
	int status = 0;
	rc = cellMusicDecodeGetDecodeStatus2(&status);
	printf("  cellMusicDecodeGetDecodeStatus2 -> 0x%08x, status=%d\n",
	       (unsigned)rc, status);

	/* Read2 */
	uint8_t buf[512];
	uint32_t startTime = 0;
	uint64_t readSize = 0;
	int position = 0;
	rc = cellMusicDecodeRead2(buf, &startTime, sizeof(buf), &readSize, &position);
	printf("  cellMusicDecodeRead2 -> 0x%08x, read=%llu pos=%d\n",
	       (unsigned)rc, (unsigned long long)readSize, position);

	/* GetContentsId2 */
	CellSearchContentId *cid = NULL;
	rc = cellMusicDecodeGetContentsId2(cid);
	printf("  cellMusicDecodeGetContentsId2 -> 0x%08x\n", (unsigned)rc);

	/* Finalize2 */
	rc = cellMusicDecodeFinalize2();
	printf("  cellMusicDecodeFinalize2 -> 0x%08x\n", (unsigned)rc);

	cellSysutilUnregisterCallback(0);
	printf("  callbacks received: %d\n", g_cb_count);
	printf("result: PASS (validation complete)\n");

	sys_process_exit(0);
	return 0;
}
