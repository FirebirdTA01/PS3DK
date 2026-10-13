/*
 * tests/sdk/sysutil-path-forwarders-test.c
 *
 * The thirteen sysutil-tree forwarder headers each forward to the cell/
 * header that already carries the surface:
 *   sysutil_ap -> cell/sysutil_ap,  sysutil_avc2 -> cell/sysutil_avc2,
 *   sysutil_avc2_video -> cell/sysutil_avc2_video, sysutil_bgmplayback ->
 *   cell/sysutil_bgmplayback, sysutil_gameupdate -> cell/sysutil_gameupdate,
 *   sysutil_music -> cell/sysutil_music, sysutil_music2 -> cell/sysutil_music
 *   (its cellMusic*2 surface lives there), sysutil_music_decode ->
 *   cell/sysutil_music_decode, sysutil_music_export -> cell/sysutil_music_export,
 *   sysutil_oskdialog_ext -> cell/sysutil_oskdialog_ext,
 *   sysutil_search_types -> cell/sysutil_search_types, sysutil_storagedata ->
 *   cell/sysutil_storagedata, sysutil_sysconf -> cell/sysutil_sysconf.
 *
 * A forwarder pointed at the wrong cell/ header leaves exactly one target
 * name undeclared and this TU fails to compile - naming the forwarder.
 *
 * Pin style (per the PATH1 review): function pointer as <fn-type> *volatile p
 * (volatile on the pointer, not the function, so it is not read as a
 * noreturn attribute; no __-prefixed names; no file-scope statements; every
 * pin referenced so -Wunused-variable cannot fire).  The two types-only
 * targets pin a type with sizeof.
 */
#include <stddef.h>

#include <sysutil/sysutil_ap.h>
#include <sysutil/sysutil_avc2.h>
#include <sysutil/sysutil_avc2_video.h>
#include <sysutil/sysutil_bgmplayback.h>
#include <sysutil/sysutil_gameupdate.h>
#include <sysutil/sysutil_music.h>
#include <sysutil/sysutil_music2.h>
#include <sysutil/sysutil_music_decode.h>
#include <sysutil/sysutil_music_export.h>
#include <sysutil/sysutil_oskdialog_ext.h>
#include <sysutil/sysutil_search_types.h>
#include <sysutil/sysutil_storagedata.h>
#include <sysutil/sysutil_sysconf.h>

/* fn-type per function-carrying target (the bare function type, so the
 * pointer below is <fn-type> *volatile). */
#define FN_AP      int(CellSysutilApParam *, sys_memory_container_t)
#define FN_AVC2    int(const CellSysutilAvc2InitParam *, uint32_t *)
#define FN_VOID    int(void)
#define FN_MUSIC   int(int, sys_memory_container_t, int, CellMusicCallback, void *)
#define FN_MDEC    int(int, sys_memory_container_t, int, CellMusicDecodeCallback, void *)
#define FN_MEXP    int(unsigned int, sys_memory_container_t, CellMusicExportUtilFinishCallback, void *)
#define FN_OSKE    int(const char **)
#define FN_SD      int(unsigned int, char *, char *, CellStorageDataSetParam *,
                      CellStorageDataFinishCallback, sys_memory_container_t, void *)
#define FN_SYSCONF int(unsigned int, CellSysconfCallback, void *, void *,
                      sys_memory_container_t)

static FN_AP      * volatile p_ap         = &cellSysutilApOn;
static FN_AVC2    * volatile p_avc2       = &cellSysutilAvc2EstimateMemoryContainerSize;
static FN_VOID    * volatile p_avc2_video = &cellSysutilAvc2ShowScreen;
static FN_VOID    * volatile p_bgm        = &cellSysutilDisableBgmPlayback;
static FN_VOID    * volatile p_gameupdate = &cellGameUpdateInit;
static FN_MUSIC   * volatile p_music      = &cellMusicInitialize;
static FN_MUSIC   * volatile p_music2     = &cellMusicInitialize2;
static FN_MDEC    * volatile p_mdec       = &cellMusicDecodeInitialize;
static FN_MEXP    * volatile p_mexp       = &cellMusicExportInitialize;
static FN_OSKE    * volatile p_oske       = &cellOskDialogExtAddJapaneseOptionDictionary;
static FN_SD      * volatile p_sd         = &cellStorageDataExport;
static FN_SYSCONF * volatile p_sysconf    = &cellSysconfOpen;

/* Types-only targets: pin a type so the name must resolve. */
static size_t pin_bgm_type    = sizeof(CellSysutilBgmPlaybackStatus);
static size_t pin_search_type = sizeof(CellSearchId);

int main(void)
{
    (void)p_ap;         (void)p_avc2;
    (void)p_avc2_video; (void)p_bgm;
    (void)p_gameupdate; (void)p_music;
    (void)p_music2;     (void)p_mdec;
    (void)p_mexp;       (void)p_oske;
    (void)p_sd;         (void)p_sysconf;
    (void)pin_bgm_type; (void)pin_search_type;
    return 0;
}
