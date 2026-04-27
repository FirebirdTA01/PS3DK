/*! \file cell/sysutil_syscache.h
 \brief Per-title scratch directory on /dev_hdd1 (cellSysCache*).

  cellSysCacheMount() reserves a per-title cache area keyed by the
  game's title-id and returns a writable path under /dev_hdd1.  The
  area persists across runs of the same title and is wiped automatically
  when a different title runs.  cellSysCacheClear() truncates it
  on demand.
*/

#ifndef PS3TC_CELL_SYSUTIL_SYSCACHE_H
#define PS3TC_CELL_SYSUTIL_SYSCACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- return / error codes ---------------------------------------- */
#define CELL_SYSCACHE_RET_OK_CLEARED      0
#define CELL_SYSCACHE_RET_OK_RELAYED      1

#define CELL_SYSCACHE_ERROR_ACCESS_ERROR  0x8002bc01
#define CELL_SYSCACHE_ERROR_INTERNAL      0x8002bc02
#define CELL_SYSCACHE_ERROR_PARAM         0x8002bc03
#define CELL_SYSCACHE_ERROR_NOTMOUNTED    0x8002bc04

/* ---- sizes ------------------------------------------------------- */
#define CELL_SYSCACHE_ID_SIZE             32
#define CELL_SYSCACHE_PATH_MAX            1055

typedef struct {
    char  cacheId[CELL_SYSCACHE_ID_SIZE];
    char  getCachePath[CELL_SYSCACHE_PATH_MAX];
    void *reserved;
} CellSysCacheParam;

extern int cellSysCacheMount(CellSysCacheParam *param);
extern int cellSysCacheClear(void);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_SYSCACHE_H */
