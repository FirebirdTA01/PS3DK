/*! \file cell/gcm.h
 \brief Sony-SDK-source-compat libgcm system surface.

  Phase 7 step-2 forwarder.  Maps the Sony cellGcm* "system function"
  surface (init / address-translation / display-buffer / flip-control /
  handlers / I/O-map management) to PSL1GHT's gcm* equivalents in
  libgcm_sys.a + librsx.

  Scope is intentionally narrow:

  - **Covered**: ~25 system functions used to bring up the RSX, register
    framebuffers, drive the flip controller, manage I/O memory, and
    install handlers.  Plus a CellGcmContextData typedef alias over
    PSL1GHT's gcmContextData (byte-identical layout — both are
    {begin, end, current, callback}).
  - **Deferred**: the entire command-emitter family (cellGcmSetSurface /
    cellGcmSetClearSurface / cellGcmSetClearColor / cellGcmSetTexture /
    cellGcmSetVertexProgram / etc., several hundred functions).  Sony's
    declarations live in <cell/gcm/gcm_command_c.h>; PSL1GHT's
    equivalents live in <rsx/commands_inc.h> under rsx* names.  These
    are inline command-buffer writers, not SPRX exports — porting them
    is mechanical but voluminous.  Until that lands, samples that need
    the command stream must call rsxFoo directly even if everything
    else is cellGcmFoo.

  Also deferred: cellGcmInit's "global current context" model.  Sony
  exposes gCellGcmCurrentContext as a global the inline command-emitters
  read; PSL1GHT exposes gGcmContext for the same purpose.  We alias the
  symbol so both names resolve to the same address.

  Function-level mapping notes:

  - cellGcmInit(cmd, io, ioAddr) wraps gcmInitBody(&gGcmContext, ...).
    Sony's flavour returns int32_t; PSL1GHT's returns s32; same width.
  - cellGcmAddressToOffset / cellGcmIoOffsetToAddress / cellGcmGetConfiguration
    / cellGcmGetTiledPitchSize / cellGcmMapMainMemory /
    cellGcmMapEaIoAddress / cellGcmUnmapIoAddress /
    cellGcmUnmapEaIoAddress / cellGcmMapLocalMemory /
    cellGcmGetMaxIoMapSize / cellGcmReserveIoMapSize /
    cellGcmUnreserveIoMapSize: 1:1 forwarders (PSL1GHT has same
    surface, identical signatures).
  - cellGcmSetDisplayBuffer / cellGcmSetFlipMode / cellGcmGetFlipStatus /
    cellGcmResetFlipStatus / cellGcmGetVBlankCount / cellGcmGetCurrentField:
    1:1 forwarders.
  - cellGcmSet{VBlank,Flip,Graphics,Queue,User,SecondV}Handler: 1:1
    forwarders.
  - cellGcmGetLabelAddress, cellGcmGetReport: 1:1 forwarders.
*/

#ifndef __PSL1GHT_CELL_GCM_H__
#define __PSL1GHT_CELL_GCM_H__

#include <stdint.h>
#include <stddef.h>
#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <cell/error.h>
#include <Cg/cg.h>
#include <Cg/cgBinary.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CellGcmContextData == gcmContextData (byte-identical: {begin, end,
 * current, callback}).  Sony's name aliased onto PSL1GHT's struct so
 * `CellGcmContextData *ctx = gCellGcmCurrentContext;` works. */
typedef gcmContextData       CellGcmContextData;
typedef gcmContextCallback   CellGcmContextCallback;

/* gcmConfiguration → CellGcmConfig: layout-equal struct with 6 fields
 * (localAddress, ioAddress, localSize, ioSize, memoryFreq/Frequency,
 * coreFreq/Frequency).  Field-name spellings differ; cast through. */
typedef gcmConfiguration     CellGcmConfig;

/* CellGcmSurface aliased onto PSL1GHT's gcmSurface.  Layouts are
 * byte-identical (same 33 fields in the same order — verified against
 * Sony's gcm_struct.h); user code that fills a CellGcmSurface and
 * passes it to cellGcmSetSurface gets reinterpreted as gcmSurface * by
 * the cellGcmSetSurface forwarder. */
typedef gcmSurface           CellGcmSurface;

/* Sony's "current context" global.  PSL1GHT exposes gGcmContext under
 * the same semantics; we re-bind via #define so user-side l-value uses
 * (gCellGcmCurrentContext->current = ..., etc.) and rvalue reads both
 * resolve to PSL1GHT's actual storage.  No symbol-aliasing tricks at
 * link time — just identifier substitution at compile. */
#define gCellGcmCurrentContext  gGcmContext

/* CELL_GCM_CURRENT macro Sony source uses to fetch the current
 * context for inline command emitters. */
#define CELL_GCM_CURRENT        gCellGcmCurrentContext

/* ============================================================
 * System functions
 * ============================================================ */

/* PSL1GHT declares gGcmContext as a 32-bit PRX pointer
 * (`gcmContextData * ATTRIBUTE_PRXPTR` == `mode(SI)`).  Taking its
 * address yields a `u32 *`, not a true `gcmContextData **`, so we
 * can't hand it to gcmInitBody directly without GCC warning.  Init
 * via a full-width local then mirror-assign with a cast so Sony source
 * that subsequently reads `gCellGcmCurrentContext` (== gGcmContext)
 * finds the same pointer PSL1GHT stored. */
static inline int32_t cellGcmInit(uint32_t cmdSize, uint32_t ioSize, void *ioAddress)
{
	gcmContextData *tmp = NULL;
	s32 rc = rsxInit(&tmp, cmdSize, ioSize, ioAddress);
	if (rc == 0) {
		gGcmContext = (gcmContextData *)(uintptr_t)tmp;
	}
	return (int32_t)rc;
}

static inline int32_t cellGcmInitSystemMode(uint64_t mode)
{
	return (int32_t)gcmInitSystemMode(mode);
}

static inline int32_t cellGcmAddressToOffset(const void *address, uint32_t *offset)
{
	return (int32_t)gcmAddressToOffset(address, offset);
}

static inline int32_t cellGcmIoOffsetToAddress(uint32_t offset, void **address)
{
	return (int32_t)gcmIoOffsetToAddress(offset, address);
}

static inline int32_t cellGcmMapMainMemory(const void *address, uint32_t size, uint32_t *offset)
{
	return (int32_t)gcmMapMainMemory(address, size, offset);
}

static inline void cellGcmGetConfiguration(CellGcmConfig *config)
{
	gcmGetConfiguration((gcmConfiguration *)config);
}

static inline uint32_t cellGcmGetTiledPitchSize(uint32_t size)
{
	return (uint32_t)gcmGetTiledPitchSize(size);
}

static inline int32_t cellGcmMapEaIoAddress(const void *ea, uint32_t io, uint32_t size)
{
	return (int32_t)gcmMapEaIoAddress(ea, io, size);
}

static inline int32_t cellGcmMapEaIoAddressWithFlags(const void *ea, uint32_t io, uint32_t size, uint32_t flags)
{
	return (int32_t)gcmMapEaIoAddressWithFlags(ea, io, size, flags);
}

static inline int32_t cellGcmUnmapIoAddress(uint32_t io)
{
	return (int32_t)gcmUnmapIoAddress(io);
}

static inline int32_t cellGcmUnmapEaIoAddress(const void *ea)
{
	return (int32_t)gcmUnmapEaIoAddress(ea);
}

static inline int32_t cellGcmMapLocalMemory(void **address, uint32_t *size)
{
	return (int32_t)gcmMapLocalMemory(address, size);
}

static inline uint32_t cellGcmGetMaxIoMapSize(void)
{
	return (uint32_t)gcmGetMaxIoMapSize();
}

/* ============================================================
 * Display / flip
 * ============================================================ */

static inline int32_t cellGcmSetDisplayBuffer(uint8_t id, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height)
{
	return (int32_t)gcmSetDisplayBuffer(id, offset, pitch, width, height);
}

static inline void cellGcmSetFlipMode(uint32_t mode)
{
	gcmSetFlipMode(mode);
}

static inline uint32_t cellGcmGetFlipStatus(void)
{
	return (uint32_t)gcmGetFlipStatus();
}

static inline void cellGcmResetFlipStatus(void)
{
	gcmResetFlipStatus();
}

/* ----------------------------------------------------------------
 * Flip APIs — three flavours.  Pick PrepareFlip + handlers; the
 * other two are advanced/dangerous.  Read this whole block before
 * using any of them.
 *
 * (1) cellGcmSetFlip(ctx, id)  — synchronous, in-stream flip.
 *     Embeds a FLIP_COMMAND directly into the GPU command buffer.
 *     The RSX swaps the displayed buffer when it processes the
 *     command.  No queue, no PPU/GPU handshake.  Sony documents the
 *     API but **does not use it in any sample**, and PSL1GHT samples
 *     don't either.
 *
 *     Observed failure mode (RPCS3, 2026-04-17): using SetFlip
 *     without supporting handlers + buffer-status labels desyncs the
 *     FIFO after the first frame.  Symptoms: GPU reads a 1.0f
 *     vertex/matrix arg as a JUMP command (because 0x3f800000 has
 *     bit-29 set), GET wanders to 0x1f800000 (unmapped), RPCS3 fires
 *     "Dead FIFO commands queue state has been detected" and the
 *     emulator hangs.  RPCS3 may suggest "Driver Wake-Up Delay" /
 *     "RSX FIFO Accuracy = Ordered & Atomic" — that papers over the
 *     symptom, not the protocol problem.  Don't ship code that
 *     relies on those settings.
 *
 *     Use SetFlip only if you (a) understand the underlying RSX
 *     PUT/GET handshake and (b) have a reason the standard
 *     PrepareFlip + handler flow won't fit.
 *
 * (2) cellGcmSetPrepareFlip(ctx, id)  — recommended.  Inserts a
 *     PREPARE_FLIP command; does NOT actually flip.  Returns a
 *     queue id 0..7, or negative when the 8-deep flip queue is
 *     full (use this as PPU back-pressure: while qid<0 sleep+retry).
 *     Pair it with:
 *       - cellGcmSetWriteBackEndLabel writing (bufferIdx<<8)|qid
 *         into a label so the VBlank handler knows which buffer to
 *         flip;
 *       - cellGcmSetVBlankHandler that reads the label and calls
 *         cellGcmSetFlipImmediate;
 *       - cellGcmSetFlipHandler that resets buffer-status labels to
 *         IDLE after the flip completes;
 *       - cellGcmSetWaitLabel + cellGcmSetWriteCommandLabel around
 *         render-target switches so the GPU itself stalls before
 *         reusing a buffer that's still being scanned out.
 *     This is the protocol Sony's flip_immediate sample uses end to
 *     end, and what samples/hello-ppu-cellgcm-triangle implements.
 *
 * (3) cellGcmSetFlipImmediate(id)  — CPU-side immediate flip, no
 *     FIFO command.  Only meant to be called from a VBlank handler
 *     after PrepareFlip has signalled a buffer is ready.  Calling
 *     it from anywhere else races against in-flight GPU work and
 *     will glitch the display.
 * ---------------------------------------------------------------- */
static inline int32_t cellGcmSetFlip(CellGcmContextData *context, uint8_t id)
{
	return (int32_t)gcmSetFlip(context, id);
}

static inline void cellGcmSetWaitFlip(CellGcmContextData *context)
{
	gcmSetWaitFlip(context);
}

static inline uint32_t cellGcmSetPrepareFlip(CellGcmContextData *context, uint8_t id)
{
	return (uint32_t)gcmSetPrepareFlip(context, id);
}

static inline int32_t cellGcmSetFlipImmediate(uint8_t id)
{
	return (int32_t)gcmSetFlipImmediate(id);
}

/* ============================================================
 * Handlers
 * ============================================================ */

static inline void cellGcmSetVBlankHandler(void (*handler)(uint32_t head))
{
	gcmSetVBlankHandler(handler);
}

static inline void cellGcmSetSecondVHandler(void (*handler)(uint32_t head))
{
	gcmSetSecondVHandler(handler);
}

static inline void cellGcmSetFlipHandler(void (*handler)(uint32_t head))
{
	gcmSetFlipHandler(handler);
}

static inline void cellGcmSetGraphicsHandler(void (*handler)(uint32_t val))
{
	gcmSetGraphicsHandler(handler);
}

static inline void cellGcmSetQueueHandler(void (*handler)(uint32_t head))
{
	gcmSetQueueHandler(handler);
}

static inline void cellGcmSetUserHandler(void (*handler)(uint32_t cause))
{
	gcmSetUserHandler(handler);
}

static inline void cellGcmSetDebugOutputLevel(int32_t level)
{
	gcmSetDebugOutputLevel((s32)level);
}

/* ============================================================
 * Hardware cursor (RSX overlay, no RSX commands needed)
 * ============================================================ */

/* Cursor texture must be 2048-byte aligned in RSX local memory.
 * Texture format is 32x32 BGRA8 = 4096 bytes per image. */
#define CELL_GCM_CURSOR_ALIGN_OFFSET   2048

static inline int32_t cellGcmInitCursor(void)
{
	return (int32_t)gcmInitCursor();
}

static inline int32_t cellGcmSetCursorEnable(void)
{
	return (int32_t)gcmSetCursorEnable();
}

static inline int32_t cellGcmSetCursorDisable(void)
{
	return (int32_t)gcmSetCursorDisable();
}

static inline int32_t cellGcmSetCursorImageOffset(uint32_t offset)
{
	return (int32_t)gcmSetCursorImageOffset(offset);
}

static inline int32_t cellGcmSetCursorPosition(int32_t x, int32_t y)
{
	return (int32_t)gcmSetCursorPosition(x, y);
}

static inline int32_t cellGcmUpdateCursor(void)
{
	return (int32_t)gcmUpdateCursor();
}

/* ============================================================
 * Reports / labels (used by samples for fence-style sync)
 * ============================================================ */

static inline uint32_t *cellGcmGetLabelAddress(uint8_t index)
{
	return gcmGetLabelAddress(index);
}

#ifdef __cplusplus
}
#endif

/* Sony-style constant spellings (CELL_GCM_*).  Header-only; lives
 * next to the GCM_* originals our runtime layer exposes. */
#include <cell/gcm/gcm_enum.h>

/* cellGcmCg* struct-walker API (libgcm_cmd.a in our stage).  Sony
 * samples include <cell/gcm.h> and call cellGcmCgInitProgram /
 * cellGcmCgGetUCode / cellGcmCgGetNamedParameter / etc. without a
 * separate include, so we pull gcm_cg_func.h in transitively. */
#include <cell/gcm/gcm_cg_func.h>

/* Command-emitter forwarders (Phase 7 step 3).  Sony source includes
 * the command-emitter family transitively through <cell/gcm.h>; we
 * preserve that. */
#include <cell/gcm/gcm_command_c.h>

/* Sony wraps the public libgcm API in the cell::Gcm C++ namespace;
 * our C-side surface already lives at global scope, so an empty
 * namespace is enough to let `using namespace cell::Gcm;` compile
 * without affecting name lookup. */
#ifdef __cplusplus
namespace cell { namespace Gcm {} }
#endif

#endif /* __PSL1GHT_CELL_GCM_H__ */
