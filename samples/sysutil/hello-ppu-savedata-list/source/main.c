/*
 * hello-ppu-savedata-list — cellSaveDataListSave2 validation test.
 *
 * Presents the savedata list dialog, offers a deterministic new slot,
 * and writes a 256-byte payload after the user selects or creates a
 * slot.  This complements hello-ppu-savedata, which covers the
 * unattended AutoSave2 path.
 *
 * Entry points exercised:
 *   cellSaveDataListSave2
 *
 * Runtime on RPCS3:
 *   - The system dialog lists existing HELSAVE02-* slots and offers
 *     "New hello-ppu-savedata-list save".
 *   - Our list callback prints the directories the utility reports,
 *     focuses the new-data row, and asks for OK_NEXT.
 *   - Our stat callback fills title / subtitle / detail metadata.
 *   - Our file callback writes SAVEDATA.BIN, then returns OK_LAST.
 *   - On HLE builds with incomplete ListSave2 support, a non-zero
 *     return or missing callback trace is reported.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/process.h>

#include <cell/sysutil_savedata.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static uint8_t g_sd_buf[64 * 1024] __attribute__((aligned(32)));
static uint8_t g_payload[256];

static const char kPrefix[] = "HELSAVE02";
static char g_new_dir_name[] = "HELSAVE02-LISTSAVE";
static char g_new_title[] = "New hello-ppu-savedata-list save";
static const char kFileName[] = "SAVEDATA.BIN";

static CellSaveDataNewDataIcon g_new_icon = {
	.title = g_new_title,
	.iconBufSize = 0,
	.iconBuf = NULL,
	.reserved = NULL,
};

static CellSaveDataListNewData g_new_data = {
	.iconPosition = CELL_SAVEDATA_ICONPOS_HEAD,
	.dirName = g_new_dir_name,
	.icon = &g_new_icon,
	.reserved = NULL,
};

static int g_list_cb_invocations = 0;
static int g_file_cb_invocations = 0;

static void fill_payload(void)
{
	for (size_t i = 0; i < sizeof(g_payload); i++) {
		g_payload[i] = (uint8_t)(0x5a ^ (i & 0xff));
	}
}

static void on_list(CellSaveDataCBResult *cbResult,
                    CellSaveDataListGet *get,
                    CellSaveDataListSet *set)
{
	g_list_cb_invocations++;
	printf("  list cb #%d: total=%u visible=%u\n",
	       g_list_cb_invocations,
	       (unsigned)get->dirNum,
	       (unsigned)get->dirListNum);

	unsigned int shown = get->dirListNum;
	if (shown > 8) {
		shown = 8;
	}
	for (unsigned int i = 0; i < shown; i++) {
		printf("    dir[%u]=%s\n", i, get->dirList[i].dirName);
	}
	fflush(stdout);

	set->focusPosition = CELL_SAVEDATA_FOCUSPOS_NEWDATA;
	set->focusDirName = g_new_dir_name;
	set->fixedListNum = get->dirListNum;
	set->fixedList = get->dirList;
	set->newData = &g_new_data;
	set->reserved = NULL;

	cbResult->result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
	cbResult->invalidMsg = NULL;
}

static void on_stat(CellSaveDataCBResult *cbResult,
                    CellSaveDataStatGet *get,
                    CellSaveDataStatSet *set)
{
	printf("  stat cb: isNew=%u freeKB=%d sizeKB=%d bind=0x%x dir=%s\n",
	       (unsigned)get->isNewData,
	       (int)get->hddFreeSizeKB,
	       (int)get->sizeKB,
	       (unsigned)get->bind,
	       get->dir.dirName);

	CellSaveDataSystemFileParam *param = &get->getParam;
	strncpy(param->title, "hello-ppu-savedata-list", sizeof(param->title) - 1);
	strncpy(param->subTitle, "ListSave2 validation", sizeof(param->subTitle) - 1);
	strncpy(param->detail,
	        "256 bytes of deterministic payload written by cellSaveDataListSave2",
	        sizeof(param->detail) - 1);
	strncpy(param->listParam, "LISTSAVE", sizeof(param->listParam));
	fflush(stdout);

	set->setParam = param;
	set->reCreateMode = CELL_SAVEDATA_RECREATE_NO;
	set->indicator = NULL;

	cbResult->result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
	cbResult->invalidMsg = NULL;
}

static void on_file(CellSaveDataCBResult *cbResult,
                    CellSaveDataFileGet *get,
                    CellSaveDataFileSet *set)
{
	g_file_cb_invocations++;
	printf("  file cb #%d: prevOpWrote=%u\n",
	       g_file_cb_invocations, (unsigned)get->excSize);
	fflush(stdout);

	if (g_file_cb_invocations == 1) {
		set->fileOperation = CELL_SAVEDATA_FILEOP_WRITE;
		set->fileType = CELL_SAVEDATA_FILETYPE_NORMALFILE;
		set->fileName = (char *)kFileName;
		set->fileOffset = 0;
		set->fileSize = (unsigned int)sizeof(g_payload);
		set->fileBufSize = (unsigned int)sizeof(g_payload);
		set->fileBuf = g_payload;
		cbResult->result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
	} else {
		cbResult->result = CELL_SAVEDATA_CBRESULT_OK_LAST;
	}

	cbResult->invalidMsg = NULL;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-savedata-list: cellSaveDataListSave2 round-trip\n");
	fflush(stdout);

	fill_payload();

	CellSaveDataSetList setList;
	memset(&setList, 0, sizeof(setList));
	setList.sortType = CELL_SAVEDATA_SORTTYPE_MODIFIEDTIME;
	setList.sortOrder = CELL_SAVEDATA_SORTORDER_DESCENT;
	setList.dirNamePrefix = (char *)kPrefix;

	CellSaveDataSetBuf setBuf;
	memset(&setBuf, 0, sizeof(setBuf));
	setBuf.dirListMax = 32;
	setBuf.fileListMax = 8;
	setBuf.bufSize = sizeof(g_sd_buf);
	setBuf.buf = g_sd_buf;

	int rc = cellSaveDataListSave2(
		CELL_SAVEDATA_VERSION_CURRENT,
		&setList,
		&setBuf,
		on_list,
		on_stat,
		on_file,
		0,
		NULL);

	printf("cellSaveDataListSave2 returned: 0x%08x\n", (unsigned)rc);
	printf("list cb fired %d times\n", g_list_cb_invocations);
	printf("file cb fired %d times\n", g_file_cb_invocations);

	int pass = (rc == CELL_SAVEDATA_RET_OK) &&
	           (g_list_cb_invocations > 0) &&
	           (g_file_cb_invocations >= 2);
	printf("result: %s\n", pass ? "PASS" : "PARTIAL-HLE");
	fflush(stdout);

	return pass ? 0 : 1;
}
