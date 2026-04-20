/*
 * hello-ppu-savedata — cellSaveDataAutoSave2 smoke test.
 *
 * Writes a 256-byte "save" into a fixed directory, using the full
 * CellSaveDataSetBuf / CellSaveDataStatSet / CellSaveDataFileSet
 * dance that a real game would follow.  The source here is intended
 * to compile against the reference sysutil_savedata.h unchanged —
 * the only PSL1GHT-ism is the #include path.
 *
 * ELF imports FNID 0x8b7ed64b (cellSaveDataAutoSave2).
 *
 * Runtime on RPCS3:
 *   - RPCS3 creates (or opens) the savedata directory under its
 *     per-game savedata root.
 *   - Our stat callback sets setParam->title/subTitle and asks for
 *     OK_NEXT.
 *   - Our file callback writes 256 bytes of known content to the
 *     slot, then asks for OK_LAST.
 *   - cellSaveDataAutoSave2 returns 0 (RET_OK).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/process.h>

#include <cell/sysutil_savedata.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* Static buffer the SPRX uses for its internal lists / file staging. */
static uint8_t g_sd_buf[32 * 1024] __attribute__((aligned(32)));

/* The file we'll write. */
static const char  kFileName[] = "SAVEDATA.BIN";
static uint8_t     g_payload[256];

static void fill_payload(void)
{
	for (size_t i = 0; i < sizeof(g_payload); i++) {
		g_payload[i] = (uint8_t)(i ^ 0xA5);
	}
}

static void on_stat(CellSaveDataCBResult *cbResult,
                    CellSaveDataStatGet  *get,
                    CellSaveDataStatSet  *set)
{
	printf("  stat cb: isNew=%u freeKB=%d sizeKB=%d bind=0x%x\n",
	       (unsigned)get->isNewData,
	       (int)get->hddFreeSizeKB,
	       (int)get->sizeKB,
	       (unsigned)get->bind);

	/* Overwrite PARAM.SFO with our title info.  For new data (isNewData
	 * == YES) this populates the fields the XMB will show; for existing
	 * data we're just refreshing. */
	CellSaveDataSystemFileParam *param = &get->getParam;
	strncpy(param->title,    "hello-ppu-savedata",
	        sizeof(param->title) - 1);
	strncpy(param->subTitle, "savedata smoke test",
	        sizeof(param->subTitle) - 1);
	strncpy(param->detail,   "256 bytes of deterministic payload written by cellSaveDataAutoSave2",
	        sizeof(param->detail) - 1);

	set->setParam     = param;
	set->reCreateMode = CELL_SAVEDATA_RECREATE_NO;
	set->indicator    = NULL;

	cbResult->result  = CELL_SAVEDATA_CBRESULT_OK_NEXT;
	cbResult->invalidMsg = NULL;
}

static int g_file_cb_invocations = 0;

static void on_file(CellSaveDataCBResult *cbResult,
                    CellSaveDataFileGet  *get,
                    CellSaveDataFileSet  *set)
{
	g_file_cb_invocations++;
	printf("  file cb #%d: prevOpWrote=%u\n",
	       g_file_cb_invocations, (unsigned)get->excSize);

	if (g_file_cb_invocations == 1) {
		/* First call — submit the write.  OK_NEXT tells the SPRX to
		 * perform this op and then call us back so we can confirm
		 * (or queue another op). */
		set->fileOperation = CELL_SAVEDATA_FILEOP_WRITE;
		set->fileType      = CELL_SAVEDATA_FILETYPE_NORMALFILE;
		set->fileName      = (char *)kFileName;
		set->fileOffset    = 0;
		set->fileSize      = (unsigned int)sizeof(g_payload);
		set->fileBufSize   = (unsigned int)sizeof(g_payload);
		set->fileBuf       = g_payload;
		cbResult->result   = CELL_SAVEDATA_CBRESULT_OK_NEXT;
	} else {
		/* Subsequent call — previous write done (prevOpWrote should be
		 * == sizeof(g_payload)).  Nothing else to write; return OK_LAST
		 * to close the utility. */
		cbResult->result = CELL_SAVEDATA_CBRESULT_OK_LAST;
	}
	cbResult->invalidMsg = NULL;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-savedata: cellSaveDataAutoSave2 round-trip\n");

	fill_payload();

	CellSaveDataSetBuf setBuf;
	memset(&setBuf, 0, sizeof(setBuf));
	setBuf.dirListMax  = 0;     /* AutoSave2 doesn't scan dir list */
	setBuf.fileListMax = 8;
	setBuf.bufSize     = sizeof(g_sd_buf);
	setBuf.buf         = g_sd_buf;

	/* Canonical pattern: "GAMEID-NAME" is 20 chars or less.
	 * RPCS3 will map this into its save-root when running headless. */
	int rc = cellSaveDataAutoSave2(
		CELL_SAVEDATA_VERSION_CURRENT,
		"HELSAVE01-AUTOSAVE",
		CELL_SAVEDATA_ERRDIALOG_NONE,
		&setBuf,
		on_stat,
		on_file,
		0 /* container — 0 = use default */,
		NULL /* userdata */);

	printf("cellSaveDataAutoSave2 returned: 0x%08x\n", (unsigned)rc);
	printf("file cb fired %d times\n", g_file_cb_invocations);
	printf("result: %s\n",
	       (rc == CELL_SAVEDATA_RET_OK) ? "PASS" : "FAIL");
	return (rc == CELL_SAVEDATA_RET_OK) ? 0 : 1;
}
