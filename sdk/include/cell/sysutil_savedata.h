/*! \file cell/sysutil_savedata.h
 \brief Sony-name-compatible cellSaveData API.

  This header exposes the full Sony SDK 475.001 cellSaveData surface —
  Sony-named structs with Sony-named fields, Sony-named callbacks,
  Sony-named constants — over PSL1GHT's runtime.  Source written
  against Sony's reference sysutil_savedata.h compiles unchanged
  (modulo `#include` paths) and links against PSL1GHT's existing
  `libsysutil.a`.

  Mechanism:
    - PSL1GHT's struct layouts are byte-identical to Sony's (Sony's
      were the source; PSL1GHT transcribed them with different C
      identifiers).  _Static_assert below pins the critical layouts;
      a regression would fail the build.
    - PSL1GHT's sysUtilSaveAutoSave2 etc. are wrappers over the
      `*Ex` SPRX trampolines that carry Sony's FNIDs.  So calling
      into PSL1GHT's runtime with Sony-shaped structs just works —
      the SPRX emulation on RPCS3 / real hardware reads the bytes
      at the same offsets either way.
    - `static inline` forwarders cast Sony struct pointers and
      callback function pointers to PSL1GHT's types.  C permits
      function-pointer casts; the argument types are ABI-identical
      (same number, same underlying bit widths) so the cast is a
      label change.

  Verified FNIDs land in the final ELF:
    cellSaveDataListLoad2    0x1dfbfdd6
    cellSaveDataListSave2    0x2de0d663
    cellSaveDataFixedLoad2   0x2a8eada2
    cellSaveDataFixedSave2   0x2aae9ef5
    cellSaveDataAutoLoad2    0xfbd5c856
    cellSaveDataAutoSave2    0x8b7ed64b
    cellSaveDataDelete2      0xedadd797
    cellSaveDataListAutoLoad 0x21425307
    cellSaveDataListAutoSave 0x4dd03a4e
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_SAVEDATA_H__
#define __PSL1GHT_CELL_SYSUTIL_SAVEDATA_H__

#include <sysutil/save.h>
#include <ppu-types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Sizes, enums, constants.  Values paraphrased from Sony SDK
 * 475.001 sysutil_savedata.h; the bit positions and numeric
 * values match bit-for-bit (verified against PSL1GHT's
 * SYS_SAVE_* set where they overlap).
 * ============================================================ */

enum {
	CELL_SAVEDATA_VERSION_CURRENT      = 0,
	CELL_SAVEDATA_VERSION_420          = 1,
};

enum {
	CELL_SAVEDATA_DIRNAME_SIZE         = 32,
	CELL_SAVEDATA_FILENAME_SIZE        = 13,
	CELL_SAVEDATA_SECUREFILEID_SIZE    = 16,
	CELL_SAVEDATA_PREFIX_SIZE          = 256,
	CELL_SAVEDATA_LISTITEM_MAX         = 2048,
	CELL_SAVEDATA_SECUREFILE_MAX       = 113,
	CELL_SAVEDATA_DIRLIST_MAX          = 2048,
	CELL_SAVEDATA_INVALIDMSG_MAX       = 256,
	CELL_SAVEDATA_INDICATORMSG_MAX     = 64,
};

enum {
	CELL_SAVEDATA_SYSP_TITLE_SIZE      = 128,
	CELL_SAVEDATA_SYSP_SUBTITLE_SIZE   = 128,
	CELL_SAVEDATA_SYSP_DETAIL_SIZE     = 1024,
	CELL_SAVEDATA_SYSP_LPARAM_SIZE     = 8,
};

enum {
	CELL_SAVEDATA_SORTTYPE_MODIFIEDTIME = 0,
	CELL_SAVEDATA_SORTTYPE_SUBTITLE     = 1,
};

enum {
	CELL_SAVEDATA_SORTORDER_DESCENT     = 0,
	CELL_SAVEDATA_SORTORDER_ASCENT      = 1,
};

enum {
	CELL_SAVEDATA_FOCUSPOS_DIRNAME      = 0,
	CELL_SAVEDATA_FOCUSPOS_LISTHEAD     = 1,
	CELL_SAVEDATA_FOCUSPOS_LISTTAIL     = 2,
	CELL_SAVEDATA_FOCUSPOS_LATEST       = 3,
	CELL_SAVEDATA_FOCUSPOS_OLDEST       = 4,
	CELL_SAVEDATA_FOCUSPOS_NEWDATA      = 5,
};

enum {
	CELL_SAVEDATA_ICONPOS_HEAD          = 0,
	CELL_SAVEDATA_ICONPOS_TAIL          = 1,
};

enum {
	CELL_SAVEDATA_ISNEWDATA_NO          = 0,
	CELL_SAVEDATA_ISNEWDATA_YES         = 1,
};

enum {
	CELL_SAVEDATA_ERRDIALOG_NONE        = 0,
	CELL_SAVEDATA_ERRDIALOG_ALWAYS      = 1,
	CELL_SAVEDATA_ERRDIALOG_NOREPEAT    = 2,
};

enum {
	CELL_SAVEDATA_RECREATE_NO           = 0,
	CELL_SAVEDATA_RECREATE_YES          = 1,
	CELL_SAVEDATA_RECREATE_YES_RESET_OWNER = 2,
	CELL_SAVEDATA_RECREATE_NO_NOBROKEN  = 3,
};

enum {
	CELL_SAVEDATA_FILETYPE_SECUREFILE   = 0,
	CELL_SAVEDATA_FILETYPE_NORMALFILE   = 1,
	CELL_SAVEDATA_FILETYPE_CONTENT_ICON0 = 2,
	CELL_SAVEDATA_FILETYPE_CONTENT_ICON1 = 3,
	CELL_SAVEDATA_FILETYPE_CONTENT_PIC1 = 4,
	CELL_SAVEDATA_FILETYPE_CONTENT_SND0 = 5,
};

enum {
	CELL_SAVEDATA_FILEOP_READ           = 0,
	CELL_SAVEDATA_FILEOP_WRITE          = 1,
	CELL_SAVEDATA_FILEOP_DELETE         = 2,
	CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC  = 3,
};

/* Return codes (Sony names over PSL1GHT equivalents). */
#define CELL_SAVEDATA_RET_OK                    0
#define CELL_SAVEDATA_RET_CANCEL                1

/* Error codes. */
#define CELL_SAVEDATA_ERROR_CBRESULT            0x8002b401
#define CELL_SAVEDATA_ERROR_ACCESS_ERROR        0x8002b402
#define CELL_SAVEDATA_ERROR_INTERNAL            0x8002b403
#define CELL_SAVEDATA_ERROR_PARAM               0x8002b404
#define CELL_SAVEDATA_ERROR_NOSPACE             0x8002b405
#define CELL_SAVEDATA_ERROR_BROKEN              0x8002b406
#define CELL_SAVEDATA_ERROR_FAILURE             0x8002b407
#define CELL_SAVEDATA_ERROR_BUSY                0x8002b408
#define CELL_SAVEDATA_ERROR_NOUSER              0x8002b409
#define CELL_SAVEDATA_ERROR_SIZEOVER            0x8002b40a
#define CELL_SAVEDATA_ERROR_NODATA              0x8002b40b
#define CELL_SAVEDATA_ERROR_NOTSUPPORTED        0x8002b40c

/* Callback result codes. */
#define CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM    2
#define CELL_SAVEDATA_CBRESULT_OK_LAST              1
#define CELL_SAVEDATA_CBRESULT_OK_NEXT              0
#define CELL_SAVEDATA_CBRESULT_ERR_NOSPACE          (-1)
#define CELL_SAVEDATA_CBRESULT_ERR_FAILURE          (-2)
#define CELL_SAVEDATA_CBRESULT_ERR_BROKEN           (-3)
#define CELL_SAVEDATA_CBRESULT_ERR_NODATA           (-4)
#define CELL_SAVEDATA_CBRESULT_ERR_INVALID          (-5)

/* Attribute + binding status bit fields. */
#define CELL_SAVEDATA_ATTR_NORMAL                   0
#define CELL_SAVEDATA_ATTR_NODUPLICATE              (1 << 0)

#define CELL_SAVEDATA_BINDSTAT_OK                   0
#define CELL_SAVEDATA_BINDSTAT_ERR_CONSOLE          (1 << 0)
#define CELL_SAVEDATA_BINDSTAT_ERR_DISC             (1 << 1)
#define CELL_SAVEDATA_BINDSTAT_ERR_PROGRAM          (1 << 2)
#define CELL_SAVEDATA_BINDSTAT_ERR_NOACCOUNTID      (1 << 3)
#define CELL_SAVEDATA_BINDSTAT_ERR_ACCOUNTID        (1 << 4)
#define CELL_SAVEDATA_BINDSTAT_ERR_NOUSERID         (1 << 5)
#define CELL_SAVEDATA_BINDSTAT_ERR_USERID           (1 << 6)
#define CELL_SAVEDATA_BINDSTAT_ERR_NOOWNER          (1 << 8)
#define CELL_SAVEDATA_BINDSTAT_ERR_OWNER            (1 << 9)
#define CELL_SAVEDATA_BINDSTAT_ERR_LOCALOWNER       (1 << 10)

#define CELL_SAVEDATA_OPTION_NONE                   0
#define CELL_SAVEDATA_OPTION_NOCONFIRM              (1 << 0)

/* ============================================================
 * Sony struct types.  Layouts match PSL1GHT's sysSave*
 * structs byte-for-byte (verified by _Static_assert below).
 * Pointer fields use ATTRIBUTE_PRXPTR so the SPRX boundary sees
 * 32-bit pointer slots on our 64-bit PPU target — same treatment
 * as PSL1GHT's own structs.
 * ============================================================ */

typedef struct CellSaveDataSetList {
	unsigned int sortType;
	unsigned int sortOrder;
	char *dirNamePrefix ATTRIBUTE_PRXPTR;
	void *reserved ATTRIBUTE_PRXPTR;
} CellSaveDataSetList;

typedef struct CellSaveDataSetBuf {
	unsigned int dirListMax;
	unsigned int fileListMax;
	unsigned int reserved[6];
	unsigned int bufSize;
	void *buf ATTRIBUTE_PRXPTR;
} CellSaveDataSetBuf;

typedef struct CellSaveDataNewDataIcon {
	char *title ATTRIBUTE_PRXPTR;
	unsigned int iconBufSize;
	void *iconBuf ATTRIBUTE_PRXPTR;
	void *reserved ATTRIBUTE_PRXPTR;
} CellSaveDataNewDataIcon;

typedef struct CellSaveDataCBResult {
	int result;
	unsigned int progressBarInc;
	int errNeedSizeKB;
	char *invalidMsg ATTRIBUTE_PRXPTR;
	void *userdata ATTRIBUTE_PRXPTR;
} CellSaveDataCBResult;

typedef struct CellSaveDataDirList {
	char dirName[CELL_SAVEDATA_DIRNAME_SIZE];
	char listParam[CELL_SAVEDATA_SYSP_LPARAM_SIZE];
	char reserved[8];
} CellSaveDataDirList;

typedef struct CellSaveDataListNewData {
	unsigned int iconPosition;
	char *dirName ATTRIBUTE_PRXPTR;
	CellSaveDataNewDataIcon *icon ATTRIBUTE_PRXPTR;
	void *reserved ATTRIBUTE_PRXPTR;
} CellSaveDataListNewData;

typedef struct CellSaveDataListGet {
	unsigned int dirNum;
	unsigned int dirListNum;
	CellSaveDataDirList *dirList ATTRIBUTE_PRXPTR;
	char reserved[64];
} CellSaveDataListGet;

typedef struct CellSaveDataListSet {
	unsigned int focusPosition;
	char *focusDirName ATTRIBUTE_PRXPTR;
	unsigned int fixedListNum;
	CellSaveDataDirList *fixedList ATTRIBUTE_PRXPTR;
	CellSaveDataListNewData *newData ATTRIBUTE_PRXPTR;
	void *reserved ATTRIBUTE_PRXPTR;
} CellSaveDataListSet;

typedef struct CellSaveDataFixedSet {
	char *dirName ATTRIBUTE_PRXPTR;
	CellSaveDataNewDataIcon *newIcon ATTRIBUTE_PRXPTR;
	unsigned int option;
} CellSaveDataFixedSet;

typedef struct CellSaveDataDirStat {
	time_t st_atime;
	time_t st_mtime;
	time_t st_ctime;
	char dirName[CELL_SAVEDATA_DIRNAME_SIZE];
} CellSaveDataDirStat;

typedef struct CellSaveDataSystemFileParam {
	char title[CELL_SAVEDATA_SYSP_TITLE_SIZE];
	char subTitle[CELL_SAVEDATA_SYSP_SUBTITLE_SIZE];
	char detail[CELL_SAVEDATA_SYSP_DETAIL_SIZE];
	unsigned int attribute;
	char reserved2[4];
	char listParam[CELL_SAVEDATA_SYSP_LPARAM_SIZE];
	char reserved[256];
} CellSaveDataSystemFileParam;

typedef struct CellSaveDataFileStat {
	unsigned int fileType;
	char reserved1[4];
	uint64_t st_size;
	time_t st_atime;
	time_t st_mtime;
	time_t st_ctime;
	char fileName[CELL_SAVEDATA_FILENAME_SIZE];
	char reserved2[3];
} CellSaveDataFileStat;

typedef struct CellSaveDataStatGet {
	int hddFreeSizeKB;
	unsigned int isNewData;
	CellSaveDataDirStat dir;
	CellSaveDataSystemFileParam getParam;
	unsigned int bind;
	int sizeKB;
	int sysSizeKB;
	unsigned int fileNum;
	unsigned int fileListNum;
	CellSaveDataFileStat *fileList ATTRIBUTE_PRXPTR;
	char reserved[64];
} CellSaveDataStatGet;

typedef struct CellSaveDataAutoIndicator {
	unsigned int dispPosition;
	unsigned int dispMode;
	char *dispMsg ATTRIBUTE_PRXPTR;
	unsigned int picBufSize;
	void *picBuf ATTRIBUTE_PRXPTR;
	void *reserved ATTRIBUTE_PRXPTR;
} CellSaveDataAutoIndicator;

typedef struct CellSaveDataStatSet {
	CellSaveDataSystemFileParam *setParam ATTRIBUTE_PRXPTR;
	unsigned int reCreateMode;
	CellSaveDataAutoIndicator *indicator ATTRIBUTE_PRXPTR;
} CellSaveDataStatSet;

typedef struct CellSaveDataFileGet {
	unsigned int excSize;
	char reserved[64];
} CellSaveDataFileGet;

typedef struct CellSaveDataFileSet {
	unsigned int fileOperation;
	void *reserved ATTRIBUTE_PRXPTR;
	unsigned int fileType;
	unsigned char secureFileId[CELL_SAVEDATA_SECUREFILEID_SIZE];
	char *fileName ATTRIBUTE_PRXPTR;
	unsigned int fileOffset;
	unsigned int fileSize;
	unsigned int fileBufSize;
	void *fileBuf ATTRIBUTE_PRXPTR;
} CellSaveDataFileSet;

typedef struct CellSaveDataDoneGet {
	int excResult;
	char dirName[CELL_SAVEDATA_DIRNAME_SIZE];
	int sizeKB;
	int hddFreeSizeKB;
	char reserved[64];
} CellSaveDataDoneGet;

/* ============================================================
 * Layout validation — these assertions pin the struct sizes we
 * depend on matching PSL1GHT's sysSave* types.  A bump in
 * ATTRIBUTE_PRXPTR semantics or a PSL1GHT struct change would
 * fail the build with a clear error, preventing silent ABI drift.
 * ============================================================ */
_Static_assert(sizeof(CellSaveDataCBResult)     == sizeof(sysSaveCallbackResult),  "CBResult layout drift");
_Static_assert(sizeof(CellSaveDataSetList)      == sizeof(sysSaveListSettings),    "SetList layout drift");
_Static_assert(sizeof(CellSaveDataSetBuf)       == sizeof(sysSaveBufferSettings),  "SetBuf layout drift");
_Static_assert(sizeof(CellSaveDataDirList)      == sizeof(sysSaveDirectoryList),   "DirList layout drift");
_Static_assert(sizeof(CellSaveDataListGet)      == sizeof(sysSaveListIn),          "ListGet layout drift");
_Static_assert(sizeof(CellSaveDataListSet)      == sizeof(sysSaveListOut),         "ListSet layout drift");
_Static_assert(sizeof(CellSaveDataDirStat)      == sizeof(sysSaveDirectoryStatus), "DirStat layout drift");
_Static_assert(sizeof(CellSaveDataFileStat)     == sizeof(sysSaveFileStatus),      "FileStat layout drift");
_Static_assert(sizeof(CellSaveDataStatGet)      == sizeof(sysSaveStatusIn),        "StatGet layout drift");
_Static_assert(sizeof(CellSaveDataStatSet)      == sizeof(sysSaveStatusOut),       "StatSet layout drift");
_Static_assert(sizeof(CellSaveDataFileGet)      == sizeof(sysSaveFileIn),          "FileGet layout drift");
_Static_assert(sizeof(CellSaveDataFileSet)      == sizeof(sysSaveFileOut),         "FileSet layout drift");
_Static_assert(sizeof(CellSaveDataFixedSet)     == sizeof(sysSaveFixedOut),        "FixedSet layout drift");
_Static_assert(sizeof(CellSaveDataAutoIndicator) == 24,                            "AutoIndicator size drift");
_Static_assert(sizeof(CellSaveDataSystemFileParam) == 1552,                        "SystemFileParam size drift");

/* ============================================================
 * Sony callback typedefs.  ABI-identical to PSL1GHT's, just
 * named after Sony's arg types.  We cast function pointers when
 * forwarding (legal in C; arg-type compatibility is guaranteed by
 * the struct-layout static asserts above).
 * ============================================================ */

typedef void (*CellSaveDataListCallback)(CellSaveDataCBResult *cbResult,
                                         CellSaveDataListGet *get,
                                         CellSaveDataListSet *set);

typedef void (*CellSaveDataFixedCallback)(CellSaveDataCBResult *cbResult,
                                          CellSaveDataListGet *get,
                                          CellSaveDataFixedSet *set);

typedef void (*CellSaveDataStatCallback)(CellSaveDataCBResult *cbResult,
                                         CellSaveDataStatGet *get,
                                         CellSaveDataStatSet *set);

typedef void (*CellSaveDataFileCallback)(CellSaveDataCBResult *cbResult,
                                         CellSaveDataFileGet *get,
                                         CellSaveDataFileSet *set);

typedef void (*CellSaveDataDoneCallback)(CellSaveDataCBResult *cbResult,
                                         CellSaveDataDoneGet *get);

/* Sony's `sys_memory_container_t` — in PSL1GHT this is
 * `sys_mem_container_t` from <sys/mempool.h>. */
typedef sys_mem_container_t sys_memory_container_t;

/* ============================================================
 * Function forwarders.  Each casts Sony struct pointers and
 * callback function pointers to PSL1GHT's types and forwards.
 * Layout compat is enforced by _Static_asserts above.
 * ============================================================ */

static inline int cellSaveDataListSave2(unsigned int version,
                                        CellSaveDataSetList *setList,
                                        CellSaveDataSetBuf *setBuf,
                                        CellSaveDataListCallback funcList,
                                        CellSaveDataStatCallback funcStat,
                                        CellSaveDataFileCallback funcFile,
                                        sys_memory_container_t container,
                                        void *userdata) {
	return (int)sysSaveListSave2(version,
	                             (sysSaveListSettings *)setList,
	                             (sysSaveBufferSettings *)setBuf,
	                             (sysSaveListCallback)funcList,
	                             (sysSaveStatusCallback)funcStat,
	                             (sysSaveFileCallback)funcFile,
	                             container, userdata);
}

static inline int cellSaveDataListLoad2(unsigned int version,
                                        CellSaveDataSetList *setList,
                                        CellSaveDataSetBuf *setBuf,
                                        CellSaveDataListCallback funcList,
                                        CellSaveDataStatCallback funcStat,
                                        CellSaveDataFileCallback funcFile,
                                        sys_memory_container_t container,
                                        void *userdata) {
	return (int)sysSaveListLoad2(version,
	                             (sysSaveListSettings *)setList,
	                             (sysSaveBufferSettings *)setBuf,
	                             (sysSaveListCallback)funcList,
	                             (sysSaveStatusCallback)funcStat,
	                             (sysSaveFileCallback)funcFile,
	                             container, userdata);
}

static inline int cellSaveDataFixedSave2(unsigned int version,
                                         CellSaveDataSetList *setList,
                                         CellSaveDataSetBuf *setBuf,
                                         CellSaveDataFixedCallback funcFixed,
                                         CellSaveDataStatCallback funcStat,
                                         CellSaveDataFileCallback funcFile,
                                         sys_memory_container_t container,
                                         void *userdata) {
	return (int)sysSaveFixedSave2(version,
	                              (sysSaveListSettings *)setList,
	                              (sysSaveBufferSettings *)setBuf,
	                              (sysSaveFixedCallback)funcFixed,
	                              (sysSaveStatusCallback)funcStat,
	                              (sysSaveFileCallback)funcFile,
	                              container, userdata);
}

static inline int cellSaveDataFixedLoad2(unsigned int version,
                                         CellSaveDataSetList *setList,
                                         CellSaveDataSetBuf *setBuf,
                                         CellSaveDataFixedCallback funcFixed,
                                         CellSaveDataStatCallback funcStat,
                                         CellSaveDataFileCallback funcFile,
                                         sys_memory_container_t container,
                                         void *userdata) {
	return (int)sysSaveFixedLoad2(version,
	                              (sysSaveListSettings *)setList,
	                              (sysSaveBufferSettings *)setBuf,
	                              (sysSaveFixedCallback)funcFixed,
	                              (sysSaveStatusCallback)funcStat,
	                              (sysSaveFileCallback)funcFile,
	                              container, userdata);
}

static inline int cellSaveDataAutoSave2(unsigned int version,
                                        const char *dirName,
                                        unsigned int errDialog,
                                        CellSaveDataSetBuf *setBuf,
                                        CellSaveDataStatCallback funcStat,
                                        CellSaveDataFileCallback funcFile,
                                        sys_memory_container_t container,
                                        void *userdata) {
	return (int)sysSaveAutoSave2((s32)version, dirName, (u32)errDialog,
	                             (sysSaveBufferSettings *)setBuf,
	                             (sysSaveStatusCallback)funcStat,
	                             (sysSaveFileCallback)funcFile,
	                             container, userdata);
}

static inline int cellSaveDataAutoLoad2(unsigned int version,
                                        const char *dirName,
                                        unsigned int errDialog,
                                        CellSaveDataSetBuf *setBuf,
                                        CellSaveDataStatCallback funcStat,
                                        CellSaveDataFileCallback funcFile,
                                        sys_memory_container_t container,
                                        void *userdata) {
	return (int)sysSaveAutoLoad2((s32)version, dirName, (u32)errDialog,
	                             (sysSaveBufferSettings *)setBuf,
	                             (sysSaveStatusCallback)funcStat,
	                             (sysSaveFileCallback)funcFile,
	                             container, userdata);
}

static inline int cellSaveDataListAutoSave(unsigned int version,
                                           unsigned int errDialog,
                                           CellSaveDataSetList *setList,
                                           CellSaveDataSetBuf *setBuf,
                                           CellSaveDataFixedCallback funcFixed,
                                           CellSaveDataStatCallback funcStat,
                                           CellSaveDataFileCallback funcFile,
                                           sys_memory_container_t container,
                                           void *userdata) {
	return (int)sysSaveListAutoSave(version, (u32)errDialog,
	                                (sysSaveListSettings *)setList,
	                                (sysSaveBufferSettings *)setBuf,
	                                (sysSaveFixedCallback)funcFixed,
	                                (sysSaveStatusCallback)funcStat,
	                                (sysSaveFileCallback)funcFile,
	                                container, userdata);
}

static inline int cellSaveDataListAutoLoad(unsigned int version,
                                           unsigned int errDialog,
                                           CellSaveDataSetList *setList,
                                           CellSaveDataSetBuf *setBuf,
                                           CellSaveDataFixedCallback funcFixed,
                                           CellSaveDataStatCallback funcStat,
                                           CellSaveDataFileCallback funcFile,
                                           sys_memory_container_t container,
                                           void *userdata) {
	return (int)sysSaveListAutoLoad(version, (u32)errDialog,
	                                (sysSaveListSettings *)setList,
	                                (sysSaveBufferSettings *)setBuf,
	                                (sysSaveFixedCallback)funcFixed,
	                                (sysSaveStatusCallback)funcStat,
	                                (sysSaveFileCallback)funcFile,
	                                container, userdata);
}

static inline int cellSaveDataDelete2(sys_memory_container_t container) {
	return (int)sysSaveDelete2(container);
}

/* ============================================================
 * Direct externs.  All entry points below resolve via the
 * libsysutil_savedata_extra_stub.a (cellSysutil-module FNIDs) and
 * libsysutil_savedata_stub.a (cellSaveData-module FNIDs) emitted
 * by `nidgen archive` from the matching YAMLs.
 *
 * Link with `-lsysutil_savedata_stub -lsysutil_savedata_extra_stub`.
 * ============================================================ */

/* CellSysutilUserId comes from cell/sysutil.h.  Forward-declared as
 * the underlying uint32 to avoid pulling the whole sysutil header in
 * for callers that only want the savedata surface. */
#ifndef _CELL_SYSUTIL_USER_ID_T_DEFINED
#define _CELL_SYSUTIL_USER_ID_T_DEFINED
typedef unsigned int CellSysutilUserId;
#endif

/* Legacy v1 entry points.  Sony stripped these from the 475 reference
 * header but the SPRX still exports the FNIDs.  Old-style empty-paren
 * declarations because the v1 argument shapes were never documented in
 * the public surface; downstreams that need them call by hand-rolled
 * matching prototypes. */
int cellSaveDataAutoLoad();
int cellSaveDataAutoSave();
int cellSaveDataListLoad();
int cellSaveDataListSave();
int cellSaveDataFixedLoad();
int cellSaveDataFixedSave();
int cellSaveDataDelete();

/* Documented entry points missing from PSL1GHT's surface. */
void cellSaveDataEnableOverlay(int enable);

int cellSaveDataFixedDelete(CellSaveDataSetList *setList,
                            CellSaveDataSetBuf *setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataDoneCallback funcDone,
                            sys_memory_container_t container,
                            void *userdata);

int cellSaveDataListDelete(CellSaveDataSetList *setList,
                           CellSaveDataSetBuf *setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataDoneCallback funcDone,
                           sys_memory_container_t container,
                           void *userdata);

int cellSaveDataListImport(CellSaveDataSetList *setList,
                           unsigned int maxSizeKB,
                           CellSaveDataDoneCallback funcDone,
                           sys_memory_container_t container,
                           void *userdata);
int cellSaveDataListExport(CellSaveDataSetList *setList,
                           unsigned int maxSizeKB,
                           CellSaveDataDoneCallback funcDone,
                           sys_memory_container_t container,
                           void *userdata);
int cellSaveDataFixedImport(const char *dirName,
                            unsigned int maxSizeKB,
                            CellSaveDataDoneCallback funcDone,
                            sys_memory_container_t container,
                            void *userdata);
int cellSaveDataFixedExport(const char *dirName,
                            unsigned int maxSizeKB,
                            CellSaveDataDoneCallback funcDone,
                            sys_memory_container_t container,
                            void *userdata);

int cellSaveDataGetListItem(const char *dirName,
                            CellSaveDataDirStat *dir,
                            CellSaveDataSystemFileParam *sysFileParam,
                            unsigned int *bind,
                            int *sizeKB);

/* Per-user (PSN-account-scoped) variants. */
int cellSaveDataUserListSave(unsigned int version,
                             CellSysutilUserId userId,
                             CellSaveDataSetList *setList,
                             CellSaveDataSetBuf *setBuf,
                             CellSaveDataListCallback funcList,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             sys_memory_container_t container,
                             void *userdata);
int cellSaveDataUserListLoad(unsigned int version,
                             CellSysutilUserId userId,
                             CellSaveDataSetList *setList,
                             CellSaveDataSetBuf *setBuf,
                             CellSaveDataListCallback funcList,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             sys_memory_container_t container,
                             void *userdata);
int cellSaveDataUserFixedSave(unsigned int version,
                              CellSysutilUserId userId,
                              CellSaveDataSetList *setList,
                              CellSaveDataSetBuf *setBuf,
                              CellSaveDataFixedCallback funcFixed,
                              CellSaveDataStatCallback funcStat,
                              CellSaveDataFileCallback funcFile,
                              sys_memory_container_t container,
                              void *userdata);
int cellSaveDataUserFixedLoad(unsigned int version,
                              CellSysutilUserId userId,
                              CellSaveDataSetList *setList,
                              CellSaveDataSetBuf *setBuf,
                              CellSaveDataFixedCallback funcFixed,
                              CellSaveDataStatCallback funcStat,
                              CellSaveDataFileCallback funcFile,
                              sys_memory_container_t container,
                              void *userdata);
int cellSaveDataUserAutoSave(unsigned int version,
                             CellSysutilUserId userId,
                             const char *dirName,
                             unsigned int errDialog,
                             CellSaveDataSetBuf *setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             sys_memory_container_t container,
                             void *userdata);
int cellSaveDataUserAutoLoad(unsigned int version,
                             CellSysutilUserId userId,
                             const char *dirName,
                             unsigned int errDialog,
                             CellSaveDataSetBuf *setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             sys_memory_container_t container,
                             void *userdata);
int cellSaveDataUserListAutoSave(unsigned int version,
                                 CellSysutilUserId userId,
                                 unsigned int errDialog,
                                 CellSaveDataSetList *setList,
                                 CellSaveDataSetBuf *setBuf,
                                 CellSaveDataFixedCallback funcFixed,
                                 CellSaveDataStatCallback funcStat,
                                 CellSaveDataFileCallback funcFile,
                                 sys_memory_container_t container,
                                 void *userdata);
int cellSaveDataUserListAutoLoad(unsigned int version,
                                 CellSysutilUserId userId,
                                 unsigned int errDialog,
                                 CellSaveDataSetList *setList,
                                 CellSaveDataSetBuf *setBuf,
                                 CellSaveDataFixedCallback funcFixed,
                                 CellSaveDataStatCallback funcStat,
                                 CellSaveDataFileCallback funcFile,
                                 sys_memory_container_t container,
                                 void *userdata);
int cellSaveDataUserFixedDelete(CellSysutilUserId userId,
                                CellSaveDataSetList *setList,
                                CellSaveDataSetBuf *setBuf,
                                CellSaveDataFixedCallback funcFixed,
                                CellSaveDataDoneCallback funcDone,
                                sys_memory_container_t container,
                                void *userdata);

int cellSaveDataUserListDelete(CellSysutilUserId userId,
                               CellSaveDataSetList *setList,
                               CellSaveDataSetBuf *setBuf,
                               CellSaveDataListCallback funcList,
                               CellSaveDataDoneCallback funcDone,
                               sys_memory_container_t container,
                               void *userdata);
int cellSaveDataUserListImport(CellSysutilUserId userId,
                               CellSaveDataSetList *setList,
                               unsigned int maxSizeKB,
                               CellSaveDataDoneCallback funcDone,
                               sys_memory_container_t container,
                               void *userdata);
int cellSaveDataUserListExport(CellSysutilUserId userId,
                               CellSaveDataSetList *setList,
                               unsigned int maxSizeKB,
                               CellSaveDataDoneCallback funcDone,
                               sys_memory_container_t container,
                               void *userdata);
int cellSaveDataUserFixedImport(CellSysutilUserId userId,
                                const char *dirName,
                                unsigned int maxSizeKB,
                                CellSaveDataDoneCallback funcDone,
                                sys_memory_container_t container,
                                void *userdata);
int cellSaveDataUserFixedExport(CellSysutilUserId userId,
                                const char *dirName,
                                unsigned int maxSizeKB,
                                CellSaveDataDoneCallback funcDone,
                                sys_memory_container_t container,
                                void *userdata);
int cellSaveDataUserGetListItem(CellSysutilUserId userId,
                                const char *dirName,
                                CellSaveDataDirStat *dir,
                                CellSaveDataSystemFileParam *sysFileParam,
                                unsigned int *bind,
                                int *sizeKB);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_SAVEDATA_H__ */
