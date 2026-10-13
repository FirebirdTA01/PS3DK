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

/* fn-type per function-carrying target: a typedef of the bare function
 * type, so the pointer below is <fn-type> *volatile. */
typedef int fn_ap(CellSysutilApParam *, sys_memory_container_t);
typedef int fn_avc2(const CellSysutilAvc2InitParam *, uint32_t *);
typedef int fn_void(void);
typedef int fn_music(int, sys_memory_container_t, int, CellMusicCallback, void *);
typedef int fn_mdec(int, sys_memory_container_t, int, CellMusicDecodeCallback, void *);
typedef int fn_mexp(unsigned int, sys_memory_container_t, CellMusicExportUtilFinishCallback, void *);
typedef int fn_oske(const char **);
typedef int fn_sd(unsigned int, char *, char *, CellStorageDataSetParam *, CellStorageDataFinishCallback, sys_memory_container_t, void *);
typedef int fn_sysconf(unsigned int, CellSysconfCallback, void *, void *, sys_memory_container_t);

static fn_ap      * volatile p_ap         = &cellSysutilApOn;
static fn_avc2    * volatile p_avc2       = &cellSysutilAvc2EstimateMemoryContainerSize;
static fn_void    * volatile p_avc2_video = &cellSysutilAvc2ShowScreen;
static fn_void    * volatile p_bgm        = &cellSysutilDisableBgmPlayback;
static fn_void    * volatile p_gameupdate = &cellGameUpdateInit;
static fn_music   * volatile p_music      = &cellMusicInitialize;
static fn_music   * volatile p_music2     = &cellMusicInitialize2;
static fn_mdec    * volatile p_mdec       = &cellMusicDecodeInitialize;
static fn_mexp    * volatile p_mexp       = &cellMusicExportInitialize;
static fn_oske    * volatile p_oske       = &cellOskDialogExtAddJapaneseOptionDictionary;
static fn_sd      * volatile p_sd         = &cellStorageDataExport;
static fn_sysconf * volatile p_sysconf    = &cellSysconfOpen;

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
