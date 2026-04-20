/*
 * PS3 Custom Toolchain — libgcm_sys legacy-name compatibility wrappers.
 *
 * Provides the PSL1GHT-flavoured `gcm*` names that <cell/gcm.h>'s
 * static-inline forwarders still emit, implemented as thin shims over the
 * cell-SDK `cellGcm*` NID stubs in our nidgen-generated libgcm_sys_stub archive.
 * The combined object — nidgen stubs + these wrappers — is installed as
 * $PS3DK/ppu/lib/libgcm_sys.a, which shadows PSL1GHT's libgcm_sys.a at
 * link time so samples pull zero bytes from the PSL1GHT install tree for
 * the GCM system surface.
 *
 * Architectural note: we are intentionally keeping the <cell/gcm.h>
 * forwarder-over-gcm* layout untouched for this transition.  As the cell
 * surface solidifies per the Phase 6/7 plan, those inlines migrate to
 * calling cellGcm* directly and this wrapper TU shrinks to nothing.
 */

#include <stdint.h>
#include <ppu-types.h>
#include <ppu-asm.h>
#include <rsx/gcm_sys.h>

/* ====================================================================
 * cellGcm* NID-bound externs — provided by libgcm_sys_stub's
 * sceStub.text / .opd.  These are the real entry points for the
 * system surface.
 * ==================================================================== */

extern int32_t  cellGcmAddressToOffset(const void *address, uint32_t *offset);
extern int32_t  cellGcmBindTile(uint8_t index);
extern int32_t  cellGcmBindZcull(uint8_t index, uint32_t offset,
                                 uint32_t width, uint32_t height,
                                 uint32_t cullStart, uint32_t zFormat,
                                 uint32_t aaFormat, uint32_t zCullDir,
                                 uint32_t zCullFormat, uint32_t sFunc,
                                 uint32_t sRef, uint32_t sMask);
extern int32_t  cellGcmDumpGraphicsError(void);
extern void     cellGcmGetConfiguration(void *config);
extern void    *cellGcmGetControlRegister(void);
extern uint32_t cellGcmGetCurrentField(void);
extern uint32_t cellGcmGetFlipStatus(void);
extern uint32_t *cellGcmGetLabelAddress(uint8_t index);
extern uint32_t cellGcmGetMaxIoMapSize(void);
extern uint32_t cellGcmGetTiledPitchSize(uint32_t size);
extern uint32_t cellGcmGetVBlankCount(void);
extern int32_t  _cellGcmInitBody(void *ATTRIBUTE_PRXPTR *ctx,
                                 uint32_t cmdSize, uint32_t ioSize,
                                 const void *ioAddress);
extern int32_t  cellGcmInitCursor(void);
extern int32_t  cellGcmInitSystemMode(uint64_t mode);
extern int32_t  cellGcmIoOffsetToAddress(uint32_t offset,
                                         void *ATTRIBUTE_PRXPTR *address);
extern int32_t  cellGcmMapEaIoAddress(const void *ea, uint32_t io, uint32_t size);
extern int32_t  cellGcmMapEaIoAddressWithFlags(const void *ea, uint32_t io,
                                               uint32_t size, uint32_t flags);
extern int32_t  cellGcmMapLocalMemory(void *ATTRIBUTE_PRXPTR *address,
                                      uint32_t *size);
extern int32_t  cellGcmMapMainMemory(const void *address, uint32_t size,
                                     uint32_t *offset);
extern int32_t  cellGcmResetFlipStatus(void);
extern int32_t  cellGcmSetCursorDisable(void);
extern int32_t  cellGcmSetCursorEnable(void);
extern int32_t  cellGcmSetCursorImageOffset(uint32_t offset);
extern int32_t  cellGcmSetCursorPosition(int32_t x, int32_t y);
extern void     cellGcmSetDebugOutputLevel(int32_t level);
extern int32_t  cellGcmSetDefaultCommandBuffer(void);
extern int32_t  cellGcmSetDisplayBuffer(uint8_t id, uint32_t offset,
                                        uint32_t pitch, uint32_t width,
                                        uint32_t height);
/* cellGcmSetFlip / cellGcmSetWaitFlip — NIDs 0xdc09357e / 0x983fb9aa, added
 * by hand to the nidgen yaml; these NIDs match PSL1GHT's gcmSetFlip /
 * gcmSetWaitFlip and resolve to working kernel entry points on RPCS3.
 * Keeping them here preserves PSL1GHT's exact runtime behavior rather
 * than substituting a speculative equivalent. */
extern int32_t  cellGcmSetFlip(void *ctx, uint8_t id);
extern void     cellGcmSetWaitFlip(void *ctx);
extern void     cellGcmSetFlipHandler(void *handler_opd32);
extern int32_t  cellGcmSetFlipImmediate(uint8_t id);
extern void     cellGcmSetFlipMode(uint32_t mode);
extern void     cellGcmSetGraphicsHandler(void *handler_opd32);
extern uint32_t cellGcmSetPrepareFlip(void *ctx, uint8_t id);
extern void     cellGcmSetQueueHandler(void *handler_opd32);
extern void     cellGcmSetSecondVHandler(void *handler_opd32);
extern int32_t  cellGcmSetTileInfo(uint8_t index, uint8_t location,
                                   uint32_t offset, uint32_t size,
                                   uint32_t pitch, uint8_t comp,
                                   uint16_t base, uint8_t bank);
extern void     cellGcmSetUserHandler(void *handler_opd32);
extern void     cellGcmSetVBlankHandler(void *handler_opd32);
extern int32_t  cellGcmUnmapEaIoAddress(const void *ea);
extern int32_t  cellGcmUnmapIoAddress(uint32_t io);
extern int32_t  cellGcmUpdateCursor(void);

/* ====================================================================
 * Legacy gcm* wrappers — thin shims that <cell/gcm.h>'s static inlines
 * still call today.  These map straight to cellGcm* variants
 * (same NID / same ABI) wherever possible; a handful need extra glue.
 * ==================================================================== */

s32 gcmAddressToOffset(const void *address, u32 *offset)
{
    return (s32)cellGcmAddressToOffset(address, offset);
}

s32 gcmBindTile(u8 index)
{
    return (s32)cellGcmBindTile(index);
}

s32 gcmBindZcull(u8 index, u32 offset, u32 width, u32 height,
                 u32 cullStart, u32 zFormat, u32 aaFormat,
                 u32 zCullDir, u32 zCullFormat, u32 sFunc,
                 u32 sRef, u32 sMask)
{
    return (s32)cellGcmBindZcull(index, offset, width, height,
                                 cullStart, zFormat, aaFormat,
                                 zCullDir, zCullFormat, sFunc, sRef, sMask);
}

s32 gcmDumpGraphicsError(void)
{
    return (s32)cellGcmDumpGraphicsError();
}

s32 gcmGetConfiguration(gcmConfiguration *config)
{
    /* cellGcmGetConfiguration fills a struct whose field layout matches
     * ours; the struct-layout PRXPTR fixup lives in <cell/gcm.h>'s
     * cellGcmGetConfiguration wrapper, not here.  For PSL1GHT-style
     * callers that hand us a 24-byte gcmConfiguration (32-bit pointers
     * via mode(SI)), the kernel writes the low 32 bits into the first
     * two fields — the zero-extension dance is performed by the
     * <cell/gcm.h> inline. */
    cellGcmGetConfiguration((void *)config);
    return 0;
}

gcmControlRegister *gcmGetControlRegister(void)
{
    return (gcmControlRegister *)cellGcmGetControlRegister();
}

u32 gcmGetCurrentField(void)
{
    return cellGcmGetCurrentField();
}

u32 gcmGetFlipStatus(void)
{
    return cellGcmGetFlipStatus();
}

u32 *gcmGetLabelAddress(u8 index)
{
    return cellGcmGetLabelAddress(index);
}

u32 gcmGetMaxIoMapSize(void)
{
    return cellGcmGetMaxIoMapSize();
}

u32 gcmGetTiledPitchSize(u32 size)
{
    return cellGcmGetTiledPitchSize(size);
}

u64 gcmGetVBlankCount(void)
{
    /* The NID stub returns uint32_t; PSL1GHT widens to u64 in the
     * public header.  Zero-extend to match PSL1GHT's signature. */
    return (u64)cellGcmGetVBlankCount();
}

/* gcmInitBodyEx/gcmInitBody — same NID as _cellGcmInitBody.  The PSL1GHT
 * typedef declares ctx as gcmContextData * ATTRIBUTE_PRXPTR * (pointer
 * to 32-bit ctx*), which matches the kernel's 32-bit-pointer world;
 * our _cellGcmInitBody extern matches. */
s32 gcmInitBodyEx(gcmContextData * ATTRIBUTE_PRXPTR *ctx,
                  const u32 cmdSize, const u32 ioSize,
                  const void *ioAddress)
{
    return (s32)_cellGcmInitBody((void * ATTRIBUTE_PRXPTR *)ctx,
                                 cmdSize, ioSize, ioAddress);
}

s32 gcmInitBody(gcmContextData **ctx, const u32 cmdSize,
                const u32 ioSize, const void *ioAddress)
{
    s32 ret;
    gcmContextData *context ATTRIBUTE_PRXPTR;

    if (ctx == NULL) return -1;

    ret = gcmInitBodyEx(&context, cmdSize, ioSize, ioAddress);
    *ctx = ret == 0 ? context : NULL;

    return ret;
}

s32 gcmInitCursor(void)
{
    return (s32)cellGcmInitCursor();
}

s32 gcmInitSystemMode(u64 mode)
{
    return (s32)cellGcmInitSystemMode(mode);
}

/* gcmIoOffsetToAddressEx/gcmIoOffsetToAddress — same dance as InitBody. */
s32 gcmIoOffsetToAddressEx(u32 offset, void * ATTRIBUTE_PRXPTR *address)
{
    return (s32)cellGcmIoOffsetToAddress(offset, address);
}

s32 gcmIoOffsetToAddress(u32 offset, void **address)
{
    s32 ret;
    void *addr ATTRIBUTE_PRXPTR;

    if (address == NULL) return -1;

    ret = gcmIoOffsetToAddressEx(offset, &addr);
    *address = ret == 0 ? addr : NULL;

    return ret;
}

s32 gcmMapEaIoAddress(const void *ea, u32 io, u32 size)
{
    return (s32)cellGcmMapEaIoAddress(ea, io, size);
}

s32 gcmMapEaIoAddressWithFlags(const void *ea, u32 io, u32 size, u32 flags)
{
    return (s32)cellGcmMapEaIoAddressWithFlags(ea, io, size, flags);
}

s32 gcmMapLocalMemory(void **address, u32 *size)
{
    s32 ret;
    void *addr ATTRIBUTE_PRXPTR;

    if (address == NULL || size == NULL) return -1;

    ret = (s32)cellGcmMapLocalMemory(&addr, size);
    *address = ret == 0 ? addr : NULL;

    return ret;
}

s32 gcmMapMainMemory(const void *address, u32 size, u32 *offset)
{
    return (s32)cellGcmMapMainMemory(address, size, offset);
}

void gcmResetFlipStatus(void)
{
    cellGcmResetFlipStatus();
}

s32 gcmSetCursorDisable(void)       { return (s32)cellGcmSetCursorDisable(); }
s32 gcmSetCursorEnable(void)        { return (s32)cellGcmSetCursorEnable(); }
s32 gcmSetCursorImageOffset(u32 o)  { return (s32)cellGcmSetCursorImageOffset(o); }
s32 gcmSetCursorPosition(s32 x, s32 y) { return (s32)cellGcmSetCursorPosition(x, y); }

void gcmSetDebugOutputLevel(s32 level)
{
    cellGcmSetDebugOutputLevel((int32_t)level);
}

void gcmSetDefaultCommandBuffer(void)
{
    (void)cellGcmSetDefaultCommandBuffer();
}

s32 gcmSetDisplayBuffer(u8 id, u32 offset, u32 pitch, u32 width, u32 height)
{
    return (s32)cellGcmSetDisplayBuffer(id, offset, pitch, width, height);
}

/* gcmSetFlip — PSL1GHT's own NID (0xdc09357e).  We added it by hand to the
 * nidgen yaml as `cellGcmSetFlip`; preserves PSL1GHT's exact runtime
 * target rather than speculatively substituting _cellGcmSetFlipCommand. */
s32 gcmSetFlip(gcmContextData *context, const u8 id)
{
    return (s32)cellGcmSetFlip((void *)context, id);
}

/* Handler setters — PSL1GHT's wrappers apply __get_opd32 to the user's
 * 64-bit function pointer before handing it to the kernel stub (the
 * cellGcmSys module's register-handler path expects the 32-bit OPD
 * descriptor form, not a raw function pointer).  Match PSL1GHT's
 * behavior exactly. */
void gcmSetFlipHandler(void (*handler)(const u32 head))
{
    cellGcmSetFlipHandler((void *)__get_opd32(handler));
}

s32 gcmSetFlipImmediate(u8 id)
{
    return (s32)cellGcmSetFlipImmediate(id);
}

void gcmSetFlipMode(const u32 mode)
{
    cellGcmSetFlipMode(mode);
}

void gcmSetGraphicsHandler(void (*handler)(const u32 val))
{
    cellGcmSetGraphicsHandler((void *)__get_opd32(handler));
}

u32 gcmSetPrepareFlip(gcmContextData *context, const u8 id)
{
    return (u32)cellGcmSetPrepareFlip((void *)context, id);
}

void gcmSetQueueHandler(void (*handler)(const u32 head))
{
    cellGcmSetQueueHandler((void *)__get_opd32(handler));
}

void gcmSetSecondVHandler(void (*handler)(const u32 head))
{
    cellGcmSetSecondVHandler((void *)__get_opd32(handler));
}

s32 gcmSetTileInfo(const u8 index, const u8 location,
                   const u32 offset, const u32 size, const u32 pitch,
                   const u8 comp, const u16 base, const u8 bank)
{
    return (s32)cellGcmSetTileInfo(index, location, offset, size, pitch,
                                   comp, base, bank);
}

void gcmSetUserHandler(void (*handler)(const u32 cause))
{
    cellGcmSetUserHandler((void *)__get_opd32(handler));
}

void gcmSetVBlankHandler(void (*handler)(const u32 head))
{
    cellGcmSetVBlankHandler((void *)__get_opd32(handler));
}

/* gcmSetWaitFlip — PSL1GHT's own NID (0x983fb9aa), added to the nidgen
 * yaml as `cellGcmSetWaitFlip`.  Same NID preserves PSL1GHT's runtime
 * target. */
void gcmSetWaitFlip(gcmContextData *context)
{
    cellGcmSetWaitFlip((void *)context);
}

s32 gcmUnmapEaIoAddress(const void *ea)
{
    return (s32)cellGcmUnmapEaIoAddress(ea);
}

s32 gcmUnmapIoAddress(u32 io)
{
    return (s32)cellGcmUnmapIoAddress(io);
}

s32 gcmUpdateCursor(void)
{
    return (s32)cellGcmUpdateCursor();
}
