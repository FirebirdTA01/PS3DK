/* cell/spurs/version.h - SPURS internal-version macros.
 *
 * Library-revision tags carried by every Initialize-with-revision
 * entry point so the SPRX can reject host headers it doesn't speak.
 */
#ifndef __PS3DK_CELL_SPURS_VERSION_H__
#define __PS3DK_CELL_SPURS_VERSION_H__

/* Already defined alongside CellSpursAttribute - re-emit guarded so
 * either include order works. */
#ifndef _CELL_SPURS_INTERNAL_VERSION
#define _CELL_SPURS_INTERNAL_VERSION       0x330000
#endif

#ifndef _CELL_SPURS_JQ_INTERNAL_VERSION
#define _CELL_SPURS_JQ_INTERNAL_VERSION    0x330000
#endif

#endif /* __PS3DK_CELL_SPURS_VERSION_H__ */
