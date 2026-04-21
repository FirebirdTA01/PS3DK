/*! \file cell/gcm.h
 \brief cell-SDK-source-compat libgcm system surface.

  Maps the cellGcm* "system function" surface (init /
  address-translation / display-buffer / flip-control / handlers /
  I/O-map management) to PSL1GHT's gcm* equivalents in libgcm_sys.a +
  librsx.

  Scope is intentionally narrow:

  - **Covered**: ~25 system functions used to bring up the RSX, register
    framebuffers, drive the flip controller, manage I/O memory, and
    install handlers.  Plus a CellGcmContextData typedef alias over
    PSL1GHT's gcmContextData (byte-identical layout — both are
    {begin, end, current, callback}).
  - **Deferred**: the entire command-emitter family (cellGcmSetSurface /
    cellGcmSetClearSurface / cellGcmSetClearColor / cellGcmSetTexture /
    cellGcmSetVertexProgram / etc., several hundred functions).  Their
    declarations live in <cell/gcm/gcm_command_c.h>; PSL1GHT's
    equivalents live in <rsx/commands_inc.h> under rsx* names.  These
    are inline command-buffer writers, not SPRX exports — porting them
    is mechanical but voluminous.  Until that lands, samples that need
    the command stream must call rsxFoo directly even if everything
    else is cellGcmFoo.

  Also deferred: cellGcmInit's "global current context" model.  The
  cell SDK exposes gCellGcmCurrentContext as a global the inline
  command-emitters read; PSL1GHT exposes gGcmContext for the same
  purpose.  We alias the symbol so both names resolve to the same
  address.

  Function-level mapping notes:

  - cellGcmInit(cmd, io, ioAddr) wraps gcmInitBody(&gGcmContext, ...).
    The cell-SDK flavour returns int32_t; PSL1GHT's returns s32; same width.
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
#include <malloc.h>        /* cell-SDK code reaches memalign via this include chain */
#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <cell/error.h>
#include <Cg/cg.h>
#include <Cg/cgBinary.h>
#include <sys/lv2_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CellGcmContextData == gcmContextData (byte-identical: {begin, end,
 * current, callback}).  Cell-SDK name aliased onto PSL1GHT's struct so
 * `CellGcmContextData *ctx = gCellGcmCurrentContext;` works. */
typedef gcmContextData       CellGcmContextData;
typedef gcmContextCallback   CellGcmContextCallback;

/* CellGcmConfig is NOT aliased to PSL1GHT's gcmConfiguration —
 * gcmConfiguration's localAddress / ioAddress are `void * __attribute__((mode(SI)))`
 * (32-bit pointers), which GCC sign-extends when loaded into a 64-bit
 * register.  A pointer like 0xc0000000 (top bit set) becomes
 * 0xffffffffc0000000 and crashes on first deref.  Our struct uses
 * full-width 64-bit pointers; our cellGcmGetConfiguration wrapper
 * zero-extends from the raw 24-byte kernel payload (see
 * CellGcmConfigRaw below). */
typedef struct {
    void     *localAddress;
    void     *ioAddress;
    uint32_t  localSize;
    uint32_t  ioSize;
    uint32_t  memoryFrequency;
    uint32_t  coreFrequency;
} CellGcmConfig;

/* CellGcmConfigRaw: the kernel-interface view of the gcm config
 * syscall payload.  Empirically 24 bytes (see Phase 3a probe —
 * docs/abi/cellos-lv2-abi-spec.md section 4.1), with the two
 * pointer-shaped fields carrying 32-bit effective addresses.
 *
 * This type exists so the widener in cellGcmGetConfiguration (below)
 * can describe the raw layout without leaning on PSL1GHT's
 * gcmConfiguration (which uses `void * mode(SI)`).  Same 24-byte
 * on-the-wire layout; cleaner C-level types. */
typedef struct {
    lv2_ea32_t localAddress_ea;
    lv2_ea32_t ioAddress_ea;
    uint32_t   localSize;
    uint32_t   ioSize;
    uint32_t   memoryFrequency;
    uint32_t   coreFrequency;
} CellGcmConfigRaw;

/* Implemented in sdk/libgcm_sys_legacy/src/gcm_legacy_wrappers.c —
 * a TU that doesn't include this header, so it can reach the nidgen
 * NID stub `cellGcmGetConfiguration` without shadowing from the
 * inline below.  Fills the 24 bytes at `raw`. */
extern void _cellGcmGetConfigurationRaw(CellGcmConfigRaw *raw);

/* CellGcmSurface is byte-identical to PSL1GHT's gcmSurface (same 33
 * fields in the same order — verified against the reference SDK's
 * gcm_struct.h); user code that fills a CellGcmSurface and passes it
 * to cellGcmSetSurface gets reinterpreted as gcmSurface * by the
 * cellGcmSetSurface forwarder.
 *
 * Defined here as our own struct (not a typedef of gcmSurface) so we
 * can expose the cell-SDK lower-case `.antialias` alongside PSL1GHT's
 * `.antiAlias` via an anonymous union without editing PSL1GHT's
 * header. */
typedef struct _cellGcmSurface {
    uint8_t  type;
    union { uint8_t antiAlias; uint8_t antialias; };
    uint8_t  colorFormat;
    uint8_t  colorTarget;
    uint8_t  colorLocation[4];    /* GCM_MAX_MRT_COUNT == 4 */
    uint32_t colorOffset[4];
    uint32_t colorPitch[4];
    uint8_t  depthFormat;
    uint8_t  depthLocation;
    uint8_t  _pad[2];
    uint32_t depthOffset;
    uint32_t depthPitch;
    uint16_t width;
    uint16_t height;
    uint16_t x;
    uint16_t y;
} CellGcmSurface;

/* CellGcmTexture aliased onto PSL1GHT's gcmTexture.  Both have the
 * same 12-field layout (format u8, mipmap u8, dimension u8, cubemap u8,
 * remap u32, width u16, height u16, depth u16, location u8, _pad u8,
 * pitch u32, offset u32) — verified against the reference SDK's gcm_struct.h. */
typedef gcmTexture           CellGcmTexture;

/* ============================================================
 * Inline helpers (non-emitter).  No FIFO writes; just math.
 * ============================================================ */

/* cellGcmAlign(a, v): round `v` up to the next multiple of `a`.
 * Matches the reference libgcm helper in <cell/gcm/gcm_helper.h>. */
static inline uint32_t cellGcmAlign(uint32_t alignment, uint32_t value)
{
    return (alignment == 0) ? value
         : (value == 0)     ? 0
         : ((((value - 1) / alignment) + 1) * alignment);
}

/* Tile region + Zcull — forward to PSL1GHT's libgcm_sys wrappers.
 * These configure the GPU's tile-memory compression and depth-cull
 * hints.  Signatures line up 1:1. */
static inline int32_t cellGcmSetTileInfo(uint8_t index, uint8_t location,
                                         uint32_t offset, uint32_t size, uint32_t pitch,
                                         uint8_t comp, uint16_t base, uint8_t bank)
{
    return (int32_t)gcmSetTileInfo(index, location, offset, size, pitch, comp, base, bank);
}

static inline int32_t cellGcmBindTile(uint8_t index)
{
    return (int32_t)gcmBindTile(index);
}

static inline int32_t cellGcmBindZcull(uint8_t index, uint32_t offset,
                                       uint32_t width, uint32_t height,
                                       uint32_t cullStart, uint32_t zFormat,
                                       uint32_t aaFormat, uint32_t zCullDir,
                                       uint32_t zCullFormat, uint32_t sFunc,
                                       uint32_t sRef, uint32_t sMask)
{
    return (int32_t)gcmBindZcull(index, offset, width, height,
                                 cullStart, zFormat, aaFormat,
                                 zCullDir, zCullFormat, sFunc, sRef, sMask);
}

/* The cell-SDK "current context" global.  PSL1GHT exposes gGcmContext
 * under the same semantics; we re-bind via #define so user-side
 * l-value uses (gCellGcmCurrentContext->current = ..., etc.) and
 * rvalue reads both resolve to PSL1GHT's actual storage.  No
 * symbol-aliasing tricks at link time — just identifier substitution
 * at compile. */
#define gCellGcmCurrentContext  gGcmContext

/* CELL_GCM_CURRENT macro cell-SDK source uses to fetch the current
 * context for inline command emitters. */
#define CELL_GCM_CURRENT        gCellGcmCurrentContext

/* ============================================================
 * System functions
 * ============================================================ */

/* PSL1GHT declares gGcmContext as a 32-bit PRX pointer
 * (`gcmContextData * ATTRIBUTE_PRXPTR` == `mode(SI)`).  Taking its
 * address yields a `u32 *`, not a true `gcmContextData **`, so we
 * can't hand it to gcmInitBody directly without GCC warning.  Init
 * via a full-width local then mirror-assign with a cast so cell-SDK
 * source that subsequently reads `gCellGcmCurrentContext` (== gGcmContext)
 * finds the same pointer PSL1GHT stored. */
/* Forward declaration: the native FIFO-wrap callback installer lives
 * in libgcm_cmd.  We override the firmware default installed by
 * rsxInit / gcmInitBodyEx because it doesn't advance PUT in the
 * wrap sequence (see <cell/gcm/ps3tc_fifo_wrap.h> for the bug
 * details). */
#include <cell/gcm/ps3tc_fifo_wrap.h>

static inline int32_t cellGcmInit(uint32_t cmdSize, uint32_t ioSize, void *ioAddress)
{
	gcmContextData *tmp = NULL;
	s32 rc = rsxInit(&tmp, cmdSize, ioSize, ioAddress);
	if (rc == 0) {
		gGcmContext = (gcmContextData *)(uintptr_t)tmp;
		ps3tc_fifo_wrap_install(tmp);
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
	if (!config) return;
	CellGcmConfigRaw raw;
	_cellGcmGetConfigurationRaw(&raw);
	/* Zero-extend the 32-bit EA fields into full 64-bit native
	 * pointers via lv2_ea32_expand — no implicit casts, no
	 * sign-extension risk.  The trailing scalar fields are
	 * straight copies. */
	config->localAddress    = lv2_ea32_expand(raw.localAddress_ea);
	config->ioAddress       = lv2_ea32_expand(raw.ioAddress_ea);
	config->localSize       = raw.localSize;
	config->ioSize          = raw.ioSize;
	config->memoryFrequency = raw.memoryFrequency;
	config->coreFrequency   = raw.coreFrequency;
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
 *     command.  No queue, no PPU/GPU handshake.  The cell SDK
 *     documents the API but **does not use it in any sample**, and
 *     PSL1GHT samples don't either.
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
 *     This is the protocol the reference flip_immediate sample uses
 *     end to end, and what samples/hello-ppu-cellgcm-triangle
 *     implements.
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

/* Cell-SDK-style constant spellings (CELL_GCM_*).  Header-only; lives
 * next to the GCM_* originals our runtime layer exposes. */
#include <cell/gcm/gcm_enum.h>

/* cellGcmCg* struct-walker API (libgcm_cmd.a in our stage).  Cell-SDK
 * samples include <cell/gcm.h> and call cellGcmCgInitProgram /
 * cellGcmCgGetUCode / cellGcmCgGetNamedParameter / etc. without a
 * separate include, so we pull gcm_cg_func.h in transitively. */
#include <cell/gcm/gcm_cg_func.h>

/* Cg -> rsx* runtime bridge.  Translates cell-SDK CGprogram /
 * CGparameter handles to PSL1GHT's rsx{Vertex,Fragment}Program-typed
 * commands so cellGcmSetVertexProgram / cellGcmSetFragmentProgram /
 * cellGcmSetVertexProgramParameter resolve against the same runtime
 * PSL1GHT's rsx_program layer already provides. */
#include <cell/gcm/gcm_cg_bridge.h>

/* Command-emitter forwarders.  Cell-SDK source includes the
 * command-emitter family transitively through <cell/gcm.h>; we
 * preserve that. */
#include <cell/gcm/gcm_command_c.h>

/* The cell SDK wraps the public libgcm API in the cell::Gcm C++
 * namespace; our C-side surface already lives at global scope, so an
 * empty namespace is enough to let `using namespace cell::Gcm;`
 * compile without affecting name lookup. */
#ifdef __cplusplus
namespace cell { namespace Gcm {} }
#endif

/* Cell-SDK-convention no-context command-emitter overloads (C++ only).
 *
 * Cell-SDK style inline emitters read the current context from the
 * global gCellGcmCurrentContext rather than taking an explicit first
 * arg.  We ship the explicit-ctx variants via gcm_command_c.h; the
 * overloads in gcm_cmd_overloads.h let the same function names compile
 * when called cell-SDK-style — they forward to the ctx variants,
 * pulling the current context from the global.
 *
 * C mode cannot overload by arity, so these stay C++-only.  C callers
 * continue to use the explicit-ctx forms.
 */
#include <cell/gcm/gcm_cmd_overloads.h>

/* SetFlip / SetPrepareFlip are declared in this file (not gcm_command_c.h);
 * hand-roll their no-context overloads to match. */
#ifdef __cplusplus
static inline int32_t cellGcmSetFlip(uint8_t id)
{ return cellGcmSetFlip(gCellGcmCurrentContext, id); }

static inline uint32_t cellGcmSetPrepareFlip(uint8_t id)
{ return cellGcmSetPrepareFlip(gCellGcmCurrentContext, id); }

static inline void cellGcmSetWaitFlip(void)
{ cellGcmSetWaitFlip(gCellGcmCurrentContext); }
#endif

#endif /* __PSL1GHT_CELL_GCM_H__ */
