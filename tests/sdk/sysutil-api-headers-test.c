/* The sysutil headers for the stub libraries that shipped without one
 * (photo decode / export / import, video export / upload, print, rec,
 * remote play, game exec, license area, cross controller, sysconf ext).
 * sysutil-api-headers-test.sh compiles this in C and C++, ILP32 and LP64,
 * and links it against the twelve stub archives: every function below must
 * resolve to a stub export of the same name.
 *
 * The layouts are written by the system modules, so they are fixed and the
 * same in both data models (measured: every size and offset below). */
#include <stddef.h>
#include <sysutil/sysutil_photo_decode.h>
#include <sysutil/sysutil_photo_export.h>
#include <sysutil/sysutil_photo_import.h>
#include <sysutil/sysutil_video_export.h>
#include <sysutil/sysutil_video_upload.h>
#include <sysutil/sysutil_print.h>
#include <sysutil/sysutil_rec.h>
#include <sysutil/sysutil_remoteplay.h>
#include <sysutil/sysutil_game_exec.h>
#include <sysutil/sysutil_game_common.h>
#include <sysutil/sysutil_licensearea.h>
#include <sysutil/sysutil_cross_controller.h>
#include <sysutil/sysutil_sysconf_ext.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif
#define LAYOUT(t, n) STATIC_ASSERT(sizeof(t) == (n), #t " is " #n " bytes")
#define AT(t, f, n) STATIC_ASSERT(offsetof(t, f) == (n), #t "." #f " is at " #n)

LAYOUT(CellSysconfBtDeviceInfo, 96);
LAYOUT(CellSysconfBtDeviceList, 1536);
LAYOUT(CellCrossControllerParam, 16);
LAYOUT(CellCrossControllerPackageInfo, 12);
LAYOUT(CellPhotoDecodeSetParam, 16);
AT(CellPhotoDecodeSetParam, reserved2, 12);
LAYOUT(CellPhotoDecodeReturnParam, 12);
AT(CellPhotoDecodeReturnParam, reserved1, 4);
LAYOUT(CellPhotoExportSetParam, 16);
LAYOUT(CellPhotoImportSetParam, 12);
LAYOUT(CellPhotoImportFileDataSub, 16);
LAYOUT(CellPhotoImportFileData, 1608);
AT(CellPhotoImportFileData, data_sub, 1600);
AT(CellPhotoImportFileData, reserved, 1604);
LAYOUT(CellVideoExportSetParam, 20);
AT(CellVideoExportSetParam, editable, 12);
LAYOUT(CellVideoUploadOption, 16);
AT(CellVideoUploadOption, value, 8);
LAYOUT(CellVideoUploadParam, 48);
AT(CellVideoUploadParam, u, 8);
AT(CellVideoUploadParam, numOfOption, 40);
AT(CellVideoUploadParam, pOption, 44);
LAYOUT(CellPrintLoadParam, 36);
LAYOUT(CellPrintStatus, 44);
LAYOUT(CellRecSpursParam, 16);
AT(CellRecSpursParam, spu_usage_rate, 4);
LAYOUT(CellRecOption, 16);
AT(CellRecOption, value, 8);
LAYOUT(CellRecParam, 16);
AT(CellRecParam, pOpt, 12);
LAYOUT(CellRecMovieMetadata, 16);
LAYOUT(CellRecSceneMetadata, 40);
AT(CellRecSceneMetadata, startTime, 8);
AT(CellRecSceneMetadata, title, 24);
AT(CellRecSceneMetadata, tagNum, 28);
AT(CellRecSceneMetadata, tag, 32);
/* The module reads the tag array as four-byte pointers: the element type is
 * four bytes in both data models, so an array of it has the module's stride. */
STATIC_ASSERT(sizeof(*((CellRecSceneMetadata *)0)->tag) == 4, "tag elements are 4-byte pointers");
int rec_two_tags(CellRecSceneMetadata *m, ps3tc_rec_tag_t *tags, char *a, char *b);
int rec_two_tags(CellRecSceneMetadata *m, ps3tc_rec_tag_t *tags, char *a, char *b)
{
	tags[0] = a;
	tags[1] = b;
	m->tagNum = 2;
	m->tag = tags;
	return (int)((char *)&m->tag[1] - (char *)&m->tag[0]);
}

/* A few values programs compare against. */
STATIC_ASSERT(CELL_SYSUTIL_LICENSE_AREA_OTHER == 100, "license area other");
STATIC_ASSERT(CELL_REC_MAX_MEMORY_CONTAINER_SIZE == 16 * 1024 * 1024, "rec container on this SDK version");
STATIC_ASSERT(CELL_PRINT_ERRORSTATUS_FATAL == 0x80000014u, "print error status run");
STATIC_ASSERT(CELL_GAME_DIRNAME_SIZE == 32, "game_common sizes come from gamecontent");

/* Every function, by address, so the link resolves each one. */
typedef void (*any_fn)(void);
#define FN(f) (any_fn)(&f)
any_fn const *volatile sysutil_api_table;
static const any_fn table[] = {
	FN(cellPhotoDecodeInitialize), FN(cellPhotoDecodeInitialize2), FN(cellPhotoDecodeFromFile),
	FN(cellPhotoDecodeFinalize),
	FN(cellPhotoExportInitialize), FN(cellPhotoExportInitialize2), FN(cellPhotoExportFromFile),
	FN(cellPhotoExportFromFileWithCopy), FN(cellPhotoExportFinalize), FN(cellPhotoExportProgress),
	FN(cellPhotoImport), FN(cellPhotoImport2),
	FN(cellVideoExportInitialize), FN(cellVideoExportInitialize2), FN(cellVideoExportFromFile),
	FN(cellVideoExportFromFileWithCopy), FN(cellVideoExportFinalize), FN(cellVideoExportProgress),
	FN(cellVideoUploadInitialize),
	FN(cellPrintLoadAsync), FN(cellPrintLoadAsync2), FN(cellPrintUnloadAsync), FN(cellPrintOpenConfig),
	FN(cellPrintStartJob), FN(cellPrintEndJob), FN(cellPrintCancelJob), FN(cellPrintStartPage),
	FN(cellPrintEndPage), FN(cellPrintGetPrintableArea), FN(cellPrintSendBand), FN(cellPrintGetStatus),
	FN(cellRecQueryMemSize), FN(cellRecOpen), FN(cellRecStart), FN(cellRecStop), FN(cellRecClose),
	FN(cellRecGetInfo), FN(cellRecSetInfo),
	FN(cellRemotePlayGetStatus), FN(cellRemotePlayGetComparativeVolume),
	FN(cellRemotePlaySetComparativeVolume), FN(cellRemotePlayBreak),
	FN(cellGameGetBootGameInfo), FN(cellGameSetExitParam), FN(cellGameGetHomeLaunchOptionPath),
	FN(cellGameGetHomeDataExportPath), FN(cellGameGetHomeDataImportPath),
	FN(cellSysutilGetLicenseArea),
	FN(cellCrossControllerInitialize),
	FN(cellSysconfBtGetDeviceList),
};

int main(void)
{
	sysutil_api_table = table;
	return (int)(sizeof table / sizeof table[0]) == 50 ? 0 : 1;
}
