/*
 * PS3 Custom Toolchain - <cell/gcm/ps3tc_gcm_flip.h>: the cellGcm flip entry points.
 *
 * cellGcmSetFlip, cellGcmSetPrepareFlip, cellGcmGetFlipStatus,
 * cellGcmResetFlipStatus, cellGcmSetWaitFlip and cellGcmSetFlipMode are the
 * primary entry points over the libgcm_sys firmware calls (the NID stubs of
 * the same names), with their Cell-style signatures (CellGcmContextData *).
 * The PSL1GHT spellings gcmSetFlip, gcmSetPrepareFlip, gcmGetFlipStatus,
 * gcmResetFlipStatus, gcmSetWaitFlip and gcmSetFlipMode (rsx/gcm_sys.h) are
 * typed aliases onto the same implementation, with their PSL1GHT signatures
 * (gcmContextData *), never the other way round.
 *
 * Included by <rsx/gcm_sys.h> after its declarations, so both <cell/gcm.h>
 * and PSL1GHT code that includes only <rsx/gcm_sys.h> see one definition.
 *
 * Each call reaches its stub through an assembler-name declaration, so the
 * generated call is the same stub call with the same arguments the gcm*
 * library shims make. The cellGcm inlines carry distinct assembler names, so
 * none can bind to the stub it calls.
 *
 * In a PS3_PROF_AUTO translation unit the observation hooks sit in the shared
 * implementation (ps3tc_gcm_flip_*): an accepted flip or prepared flip is
 * reported once as a frame boundary and each flip-status read is reported as
 * a poll, whichever spelling the code uses.
 */
#ifndef PS3TC_CELL_GCM_FLIP_H
#define PS3TC_CELL_GCM_FLIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CellGcmContextData == gcmContextData (byte-identical: {begin, end,
 * current, callback}). In C++ the cell headers expose a real struct tag so
 * both `CellGcmContextData *` and `struct CellGcmContextData *` spellings
 * compile, while still converting to the underlying RSX context type. */
#ifndef PS3TC_CELL_GCM_CONTEXT_DATA
#define PS3TC_CELL_GCM_CONTEXT_DATA
#ifdef __cplusplus
struct CellGcmContextData : public gcmContextData {};
#else
typedef gcmContextData CellGcmContextData;
#endif
#endif

/* The firmware calls (libgcm_sys NID stubs). */
int32_t  ps3tc_gcm_sys_SetFlip(struct _gcmCtxData *context, uint8_t id) __asm__("cellGcmSetFlip");
uint32_t ps3tc_gcm_sys_SetPrepareFlip(struct _gcmCtxData *context, uint8_t id) __asm__("cellGcmSetPrepareFlip");
uint32_t ps3tc_gcm_sys_GetFlipStatus(void) __asm__("cellGcmGetFlipStatus");
void     ps3tc_gcm_sys_ResetFlipStatus(void) __asm__("cellGcmResetFlipStatus");
void     ps3tc_gcm_sys_SetWaitFlip(struct _gcmCtxData *context) __asm__("cellGcmSetWaitFlip");
void     ps3tc_gcm_sys_SetFlipMode(uint32_t mode) __asm__("cellGcmSetFlipMode");

#if defined(PS3TC_PROF_AUTO) && !defined(PS3TC_PROF_AUTO_NO_FLIP_HOOK)
/* PS3_PROF_AUTO observation (libps3dkprof_auto.a); see ps3tc_prof_auto.h. */
void     ps3dk_prof__auto_flip(struct _gcmCtxData *context, int32_t ok);
uint64_t ps3dk_prof__auto_flip_status_begin(void);
void     ps3dk_prof__auto_flip_status_end(uint64_t before, uint32_t status);
uint64_t ps3dk_prof__auto_flip_wait_begin(void);
void     ps3dk_prof__auto_flip_wait_end(uint64_t scope, int32_t rc);
#define PS3TC_GCM_FLIP_OBSERVED 1
#endif

/* The shared implementation: one firmware call, and the observation, once. */
static inline int32_t ps3tc_gcm_flip_SetFlip(struct _gcmCtxData *context, uint8_t id)
{
	int32_t rc = ps3tc_gcm_sys_SetFlip(context, id);
#ifdef PS3TC_GCM_FLIP_OBSERVED
	ps3dk_prof__auto_flip(context, rc == 0);
#endif
	return rc;
}

static inline uint32_t ps3tc_gcm_flip_SetPrepareFlip(struct _gcmCtxData *context, uint8_t id)
{
	uint32_t queue = ps3tc_gcm_sys_SetPrepareFlip(context, id);
#ifdef PS3TC_GCM_FLIP_OBSERVED
	ps3dk_prof__auto_flip(context, (int32_t)queue >= 0);
#endif
	return queue;
}

static inline uint32_t ps3tc_gcm_flip_GetFlipStatus(void)
{
#ifdef PS3TC_GCM_FLIP_OBSERVED
	uint64_t before = ps3dk_prof__auto_flip_status_begin();
	uint32_t status = ps3tc_gcm_sys_GetFlipStatus();
	ps3dk_prof__auto_flip_status_end(before, status);
	return status;
#else
	return ps3tc_gcm_sys_GetFlipStatus();
#endif
}

/* The cellGcm entry points (Cell-style signatures). */
static inline int32_t cellGcmSetFlip(CellGcmContextData *context, uint8_t id) __asm__("ps3tc_inline_cellGcmSetFlip");
static inline int32_t cellGcmSetFlip(CellGcmContextData *context, uint8_t id)
{
	return ps3tc_gcm_flip_SetFlip(context, id);
}

static inline uint32_t cellGcmSetPrepareFlip(CellGcmContextData *context, uint8_t id) __asm__("ps3tc_inline_cellGcmSetPrepareFlip");
static inline uint32_t cellGcmSetPrepareFlip(CellGcmContextData *context, uint8_t id)
{
	return ps3tc_gcm_flip_SetPrepareFlip(context, id);
}

static inline uint32_t cellGcmGetFlipStatus(void) __asm__("ps3tc_inline_cellGcmGetFlipStatus");
static inline uint32_t cellGcmGetFlipStatus(void)
{
	return ps3tc_gcm_flip_GetFlipStatus();
}

static inline void cellGcmResetFlipStatus(void) __asm__("ps3tc_inline_cellGcmResetFlipStatus");
static inline void cellGcmResetFlipStatus(void)
{
	ps3tc_gcm_sys_ResetFlipStatus();
}

/* Not a frame boundary: the wait is a GPU command, not an owner wait. */
static inline void cellGcmSetWaitFlip(CellGcmContextData *context) __asm__("ps3tc_inline_cellGcmSetWaitFlip");
static inline void cellGcmSetWaitFlip(CellGcmContextData *context)
{
	ps3tc_gcm_sys_SetWaitFlip(context);
}

static inline void cellGcmSetFlipMode(uint32_t mode) __asm__("ps3tc_inline_cellGcmSetFlipMode");
static inline void cellGcmSetFlipMode(uint32_t mode)
{
	ps3tc_gcm_sys_SetFlipMode(mode);
}

#ifdef __cplusplus
}
#endif

#endif
