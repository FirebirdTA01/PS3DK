/*
 * PS3 Custom Toolchain — libgcm_sys legacy-name compatibility wrappers.
 *
 * Provides the PSL1GHT-flavoured `gcm*` names that <cell/gcm.h>'s
 * static-inline forwarders still emit, implemented as thin shims over the
 * the reference SDK `cellGcm*` NID stubs in our nidgen-generated libgcm_sys_stub archive.
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
#include <sys/lv2_types.h>

/* ABI witness.  This object is compiled twice — once per ABI — and filed
 * into the ilp32 and lp64 copies of libgcm_sys.a.  Nothing in an ELF64
 * PPC64 object records which of the two it was: the class is ELF64 and
 * e_flags is 0 either way, so a build that silently dropped -mlp64 filed
 * an ILP32 object under lp64/ and nobody noticed until an LP64 caller got
 * a 4-byte stw into an 8-byte pointer output.
 *
 * The array's SIZE is the pointer width the compiler actually used, and
 * `nm -S` reads it straight out of the symbol table, so the check needs
 * no predefined macro and no disassembly.  Static, so a second TU in the
 * same archive cannot collide with it.
 * Read by tests/sdk/gcm-legacy-symbol-coverage-test.sh. */
static const char ps3tc_abi_witness_ptr[sizeof(void *)]
    __attribute__((used)) = { 0 };

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
extern int32_t  cellGcmGetCurrentDisplayBufferId(uint8_t *id);
extern uint32_t cellGcmGetCurrentField(void);
extern int32_t  cellGcmGetDisplayBufferByFlipIndex(const uint32_t qid);
/* Pointer-returning system calls hand back a 32-bit effective address, so
 * they are declared as lv2_ea32_t and widened through lv2_ea32_expand.
 * This is type discipline, not a codegen difference: at -O2 GCC emits the
 * same call and return sequence as the older `void *` spelling used by
 * gcmGetControlRegister and gcmGetLabelAddress, because it takes the
 * declared 32-bit return type as already ABI-extended.  What the
 * lv2_ea32_t spelling buys is that the EA-ness is stated where the value
 * crosses, and that the -mlp64 widening is written down rather than
 * implied by a cast - measured, it does not add a clear of r3's upper
 * half, and it should not be read as protecting against one. */
extern lv2_ea32_t cellGcmGetDisplayInfo(void);
extern uint32_t cellGcmGetFlipStatus(void);
extern uint32_t *cellGcmGetLabelAddress(uint8_t index);
extern int64_t  cellGcmGetLastFlipTime(void);
extern int64_t  cellGcmGetLastSecondVTime(void);
extern uint32_t cellGcmGetMaxIoMapSize(void);
extern lv2_ea32_t cellGcmGetNotifyDataAddress(const uint32_t index);
/* Fills two 32-bit EAs - 8 bytes - because the reference SDK's
 * CellGcmOffsetTable is a pair of 32-bit pointers.  Typed void * here so
 * the LP64 widening happens in the wrapper, not at this boundary. */
extern void     cellGcmGetOffsetTable(void *table);
extern uint32_t cellGcmGetReport(const uint32_t type, const uint32_t index);
extern lv2_ea32_t cellGcmGetReportDataAddress(const uint32_t index);
extern lv2_ea32_t cellGcmGetReportDataAddressLocation(const uint32_t index,
                                                      const uint32_t location);
extern uint32_t cellGcmGetReportDataLocation(const uint32_t index,
                                             const uint32_t location);
extern lv2_ea32_t cellGcmGetTileInfo(void);
extern uint32_t cellGcmGetTiledPitchSize(uint32_t size);
extern uint64_t cellGcmGetTimeStamp(const uint32_t index);
extern uint64_t cellGcmGetTimeStampLocation(const uint32_t index,
                                            const uint32_t location);
extern uint32_t cellGcmGetVBlankCount(void);
extern lv2_ea32_t cellGcmGetZcullInfo(void);
extern int32_t  _cellGcmInitBody(void *ATTRIBUTE_PRXPTR *ctx,
                                 uint32_t cmdSize, uint32_t ioSize,
                                 const void *ioAddress);
extern int32_t  cellGcmInitCursor(void);
extern int32_t  cellGcmInitDefaultFifoMode(int32_t mode);
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
extern int32_t  cellGcmReserveIoMapSize(const uint32_t size);
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
extern void     cellGcmSetFlipStatus(void);
extern void     cellGcmSetFlipMode(uint32_t mode);
extern void     cellGcmSetGraphicsHandler(void *handler_opd32);
extern void     cellGcmSetInvalidateTile(const uint8_t index);
extern uint32_t cellGcmSetPrepareFlip(void *ctx, uint8_t id);
extern void     cellGcmSetQueueHandler(void *handler_opd32);
extern void     cellGcmSetSecondVFrequency(const uint32_t freq);
extern void     cellGcmSetSecondVHandler(void *handler_opd32);
extern void     cellGcmSetTile(const uint8_t index, const uint8_t location,
                               const uint32_t offset, const uint32_t size,
                               const uint32_t pitch, const uint8_t comp,
                               const uint16_t base, const uint8_t bank);
extern int32_t  cellGcmSetTileInfo(uint8_t index, uint8_t location,
                                   uint32_t offset, uint32_t size,
                                   uint32_t pitch, uint8_t comp,
                                   uint16_t base, uint8_t bank);
extern void     cellGcmSetUserHandler(void *handler_opd32);
extern void     cellGcmSetVBlankFrequency(const uint32_t freq);
extern void     cellGcmSetVBlankHandler(void *handler_opd32);
extern void     cellGcmSetZcull(const uint8_t index, const uint32_t offset,
                                const uint32_t width, const uint32_t height,
                                const uint32_t cullStart,
                                const uint32_t zFormat, const uint32_t aaFormat,
                                const uint32_t zCullDir,
                                const uint32_t zCullFormat, const uint32_t sFunc,
                                const uint32_t sRef, const uint32_t sMask);
extern int32_t  cellGcmSortRemapEaIoAddress(void);
extern int32_t  cellGcmUnbindTile(const uint8_t index);
extern int32_t  cellGcmUnbindZcull(const uint8_t index);
extern int32_t  cellGcmUnmapEaIoAddress(const void *ea);
extern int32_t  cellGcmUnmapIoAddress(uint32_t io);
extern int32_t  cellGcmUnreserveIoMapSize(const uint32_t size);
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

/* _cellGcmGetConfigurationRaw: type-neutral variant for cell/gcm.h's
 * widener. The header declares CellGcmConfigRaw (24 bytes, lv2_ea32_t
 * for the two EA fields) and asks us to fill it. Lv-2 writes exactly
 * 24 bytes (empirically confirmed via samples/toolchain/gcm-config-abi
 * under RPCS3; see docs/abi/cellos-lv2-abi-spec.md section 4.1), so
 * a void * at this boundary is sufficient. This path avoids leaning
 * on PSL1GHT's gcmConfiguration type (void * mode(SI)) in our inline.
 *
 * Symbol name is `_cellGcmGetConfigurationRaw` — underscore-prefixed
 * to mark it as internal detail of the cellGcmGetConfiguration
 * widener, not a caller-facing API. */
void _cellGcmGetConfigurationRaw(void *raw24)
{
    cellGcmGetConfiguration(raw24);
}

gcmControlRegister *gcmGetControlRegister(void)
{
    return (gcmControlRegister *)cellGcmGetControlRegister();
}

s32 gcmGetCurrentDisplayBufferId(u8 *id)
{
    return (s32)cellGcmGetCurrentDisplayBufferId(id);
}

u32 gcmGetCurrentField(void)
{
    return cellGcmGetCurrentField();
}

s32 gcmGetDisplayBufferByFlipIndex(const u32 qid)
{
    return (s32)cellGcmGetDisplayBufferByFlipIndex(qid);
}

const gcmDisplayInfo *gcmGetDisplayInfo(void)
{
    return (const gcmDisplayInfo *)lv2_ea32_expand(cellGcmGetDisplayInfo());
}

u32 gcmGetFlipStatus(void)
{
    return cellGcmGetFlipStatus();
}

u32 *gcmGetLabelAddress(u8 index)
{
    return cellGcmGetLabelAddress(index);
}

s64 gcmGetLastFlipTime(void)
{
    return (s64)cellGcmGetLastFlipTime();
}

s64 gcmGetLastSecondVTime(void)
{
    return (s64)cellGcmGetLastSecondVTime();
}

u32 gcmGetMaxIoMapSize(void)
{
    return cellGcmGetMaxIoMapSize();
}

gcmNotifyData *gcmGetNotifyDataAddress(const u32 index)
{
    return (gcmNotifyData *)lv2_ea32_expand(cellGcmGetNotifyDataAddress(index));
}

/* The kernel writes two 32-bit EAs - 8 bytes - because the reference SDK's
 * CellGcmOffsetTable is a pair of 32-bit pointers.  Under -mlp64 our
 * gcmOffsetTable is 16 bytes with 8-byte members, so the raw pair is
 * received into a fixed-width local and widened field by field.  Under
 * ILP32 the widening is the identity and the local costs one copy. */
void gcmGetOffsetTable(gcmOffsetTable *table)
{
    struct { lv2_ea32_t io; lv2_ea32_t ea; } raw = { 0, 0 };

    if (table == NULL) return;

    cellGcmGetOffsetTable(&raw);
    table->io = (u16 *)lv2_ea32_expand(raw.io);
    table->ea = (u16 *)lv2_ea32_expand(raw.ea);
}

u32 gcmGetReport(const u32 type, const u32 index)
{
    return cellGcmGetReport(type, index);
}

gcmReportData *gcmGetReportDataAddress(const u32 index)
{
    return (gcmReportData *)lv2_ea32_expand(cellGcmGetReportDataAddress(index));
}

gcmReportData *gcmGetReportDataAddressLocation(const u32 index,
                                               const u32 location)
{
    return (gcmReportData *)
        lv2_ea32_expand(cellGcmGetReportDataAddressLocation(index, location));
}

u32 gcmGetReportDataLocation(const u32 index, const u32 location)
{
    return cellGcmGetReportDataLocation(index, location);
}

const gcmTileInfo *gcmGetTileInfo(void)
{
    return (const gcmTileInfo *)lv2_ea32_expand(cellGcmGetTileInfo());
}

u32 gcmGetTiledPitchSize(u32 size)
{
    return cellGcmGetTiledPitchSize(size);
}

u64 gcmGetTimeStamp(const u32 index)
{
    return cellGcmGetTimeStamp(index);
}

u64 gcmGetTimeStampLocation(const u32 index, const u32 location)
{
    return cellGcmGetTimeStampLocation(index, location);
}

u64 gcmGetVBlankCount(void)
{
    /* The NID stub returns uint32_t; PSL1GHT widens to u64 in the
     * public header.  Zero-extend to match PSL1GHT's signature. */
    return (u64)cellGcmGetVBlankCount();
}

const gcmZcullInfo *gcmGetZcullInfo(void)
{
    return (const gcmZcullInfo *)lv2_ea32_expand(cellGcmGetZcullInfo());
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

s32 gcmInitDefaultFifoMode(s32 mode)
{
    return (s32)cellGcmInitDefaultFifoMode((int32_t)mode);
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

s32 gcmReserveIoMapSize(const u32 size)
{
    return (s32)cellGcmReserveIoMapSize(size);
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

/* Handler setters — the kernel-visible callback registration path
 * expects a 32-bit EA pointing at an 8-byte compact descriptor
 * (entry_ea, toc_ea). Under our native compact-OPD ABI a C function
 * pointer already IS that EA, so lv2_fn_to_callback_ea is a bare cast
 * (typed for clarity at the boundary). Replaces PSL1GHT's __get_opd32
 * + (descriptor + 16) form; see docs/abi/compact-opd-migration.md. */
void gcmSetFlipHandler(void (*handler)(const u32 head))
{
    cellGcmSetFlipHandler((void *)(uintptr_t)lv2_fn_to_callback_ea(handler));
}

s32 gcmSetFlipImmediate(u8 id)
{
    return (s32)cellGcmSetFlipImmediate(id);
}

void gcmSetFlipMode(const u32 mode)
{
    cellGcmSetFlipMode(mode);
}

void gcmSetFlipStatus(void)
{
    cellGcmSetFlipStatus();
}

void gcmSetInvalidateTile(const u8 index)
{
    cellGcmSetInvalidateTile(index);
}

void gcmSetGraphicsHandler(void (*handler)(const u32 val))
{
    cellGcmSetGraphicsHandler((void *)(uintptr_t)lv2_fn_to_callback_ea(handler));
}

u32 gcmSetPrepareFlip(gcmContextData *context, const u8 id)
{
    return (u32)cellGcmSetPrepareFlip((void *)context, id);
}

void gcmSetQueueHandler(void (*handler)(const u32 head))
{
    cellGcmSetQueueHandler((void *)(uintptr_t)lv2_fn_to_callback_ea(handler));
}

void gcmSetSecondVHandler(void (*handler)(const u32 head))
{
    cellGcmSetSecondVHandler((void *)(uintptr_t)lv2_fn_to_callback_ea(handler));
}

void gcmSetSecondVFrequency(const u32 freq)
{
    cellGcmSetSecondVFrequency(freq);
}

void gcmSetTile(const u8 index, const u8 location, const u32 offset,
                const u32 size, const u32 pitch, const u8 comp,
                const u16 base, const u8 bank)
{
    cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank);
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
    cellGcmSetUserHandler((void *)(uintptr_t)lv2_fn_to_callback_ea(handler));
}

void gcmSetVBlankFrequency(const u32 freq)
{
    cellGcmSetVBlankFrequency(freq);
}

void gcmSetVBlankHandler(void (*handler)(const u32 head))
{
    cellGcmSetVBlankHandler((void *)(uintptr_t)lv2_fn_to_callback_ea(handler));
}

/* gcmSetWaitFlip — PSL1GHT's own NID (0x983fb9aa), added to the nidgen
 * yaml as `cellGcmSetWaitFlip`.  Same NID preserves PSL1GHT's runtime
 * target. */
void gcmSetWaitFlip(gcmContextData *context)
{
    cellGcmSetWaitFlip((void *)context);
}

void gcmSetZcull(const u8 index, const u32 offset, const u32 width,
                 const u32 height, const u32 cullStart, const u32 zFormat,
                 const u32 aaFormat, const u32 zCullDir,
                 const u32 zCullFormat, const u32 sFunc, const u32 sRef,
                 const u32 sMask)
{
    cellGcmSetZcull(index, offset, width, height, cullStart, zFormat,
                    aaFormat, zCullDir, zCullFormat, sFunc, sRef, sMask);
}

s32 gcmSortRemapEaIoAddress(void)
{
    return (s32)cellGcmSortRemapEaIoAddress();
}

s32 gcmUnbindTile(const u8 index)
{
    return (s32)cellGcmUnbindTile(index);
}

s32 gcmUnbindZcull(const u8 index)
{
    return (s32)cellGcmUnbindZcull(index);
}

s32 gcmUnmapEaIoAddress(const void *ea)
{
    return (s32)cellGcmUnmapEaIoAddress(ea);
}

s32 gcmUnmapIoAddress(u32 io)
{
    return (s32)cellGcmUnmapIoAddress(io);
}

s32 gcmUnreserveIoMapSize(const u32 size)
{
    return (s32)cellGcmUnreserveIoMapSize(size);
}

s32 gcmUpdateCursor(void)
{
    return (s32)cellGcmUpdateCursor();
}
