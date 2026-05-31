/* psgl_command_buffer.c -- Static Command Buffer implementation
 *
 * Implements the 10 psgl command-buffer APIs from PSGL/psgl.h.
 * Keeps FIFO recording state separate from GL draw-state (psgl_context.c).
 */
#include <PSGL/psgl.h>
#include <stdint.h>
#include <malloc.h>
#include <string.h>

#include "psgl_command_buffer.h"
#include "psgl_context.h"

/* recording state */
static GLboolean g_record_mode = GL_FALSE;
static CellGcmContextData *g_record_prev_context;
static uint32_t *g_record_prev_begin;
static uint32_t *g_record_prev_end;
static uint32_t *g_record_prev_current;
static psglStaticCommandBufferCallBack g_out_of_space_cb;
static psglStallCallBack g_stall_cb;

/* helpers */

static void psgl_cb_save_context(CellGcmContextData *ctx)
{
    g_record_prev_context = ctx;
    g_record_prev_begin = ctx->begin;
    g_record_prev_end = ctx->end;
    g_record_prev_current = ctx->current;
}

static void psgl_cb_restore_context(CellGcmContextData *ctx)
{
    ctx->begin = g_record_prev_begin;
    ctx->end = g_record_prev_end;
    ctx->current = g_record_prev_current;
}

/* inline unsafe jump-to-self for SPU-hole mechanism */
static void psgl_cb_jump_to_self(CellGcmContextData *ctx)
{
    uint32_t off;
    cellGcmAddressToOffset(ctx->current, &off);
    ctx->current[0] = off | 0x20000000u; /* JUMP flag */
}

/* public API */

PSGL_EXPORT void psglBeginCommandRecord(void *commandBuffer,
                                        GLuint sizeInBytes)
{
    CellGcmContextData *ctx;
    if (!commandBuffer || sizeInBytes < 32u) return;
    ctx = gCellGcmCurrentContext;
    if (!ctx) return;

    cellGcmFlush(ctx);
    psgl_cb_save_context(ctx);

    ctx->begin = (uint32_t *)commandBuffer;
    ctx->current = (uint32_t *)commandBuffer;
    ctx->end = (uint32_t *)((uint8_t *)commandBuffer + sizeInBytes);

    /* 8 NOP dwords at start (GET/PUT equality workaround) */
    for (int i = 0; i < 8; i++)
        ctx->current[i] = 0u;
    ctx->current += 8;

    g_record_mode = GL_TRUE;
}

PSGL_EXPORT void *psglGetCommandRecordCurrent(void)
{
    CellGcmContextData *ctx;
    if (!g_record_mode) return NULL;
    ctx = gCellGcmCurrentContext;
    return ctx ? ctx->current : NULL;
}

PSGL_EXPORT void *psglEndCommandRecord(bool addReturn)
{
    CellGcmContextData *ctx;
    uint32_t *end_pos;
    if (!g_record_mode) return NULL;
    ctx = gCellGcmCurrentContext;
    if (!ctx) return NULL;

    if (addReturn)
        cellGcmSetReturnCommand(ctx);

    end_pos = ctx->current;
    psgl_cb_restore_context(ctx);
    g_record_mode = GL_FALSE;
    return end_pos;
}

PSGL_EXPORT void *psglCallCommandBuffer(void *commandBuffer)
{
    CellGcmContextData *ctx;
    uint32_t offset;
    uint32_t *prev;
    if (!commandBuffer) return NULL;
    ctx = gCellGcmCurrentContext;
    if (!ctx) return NULL;

    cellGcmAddressToOffset(commandBuffer, &offset);
    prev = ctx->current;
    cellGcmSetCallCommand(ctx, offset);
    return prev;
}

PSGL_EXPORT void psglPushCommandBuffer(void *commandBuffer,
                                       GLuint sizeInBytes)
{
    CellGcmContextData *ctx;
    if (!commandBuffer || !sizeInBytes) return;
    ctx = gCellGcmCurrentContext;
    if (!ctx) return;
    if (ctx->current + (sizeInBytes >> 2u) > ctx->end) return;
    memcpy(ctx->current, commandBuffer, (size_t)sizeInBytes);
    ctx->current += (sizeInBytes + 3u) >> 2u;
}

PSGL_EXPORT void *psglAllocateCommandBuffer(GLuint sizeInBytes,
                                            GLuint *offset)
{
    uint32_t rounded;
    void *mem;
    if (!sizeInBytes) return NULL;
    rounded = (sizeInBytes + 0xFFFFFu) & ~0xFFFFFu;
    if (rounded < sizeInBytes) return NULL;
    mem = memalign(1024u * 1024u, (size_t)rounded);
    if (!mem) return NULL;
    memset(mem, 0, (size_t)rounded);
    if (cellGcmMapMainMemory(mem, (size_t)rounded, offset) != 0) {
        free(mem);
        return NULL;
    }
    return mem;
}

PSGL_EXPORT void psglFreeCommandBuffer(void *commandBuffer)
{
    free(commandBuffer);
}

PSGL_EXPORT void psglSetRecordOutOfSpaceCallback(
    psglStaticCommandBufferCallBack callback)
{
    g_out_of_space_cb = callback;
}

PSGL_EXPORT void psglSetStallCallback(psglStallCallBack callBack)
{
    g_stall_cb = callBack;
}

PSGL_EXPORT int psglDrawCommandBufferHole(
    GLenum mode, GLsizei count, GLenum type,
    const GLvoid *indices, uint32_t *indexOffset,
    uint32_t *holeEA, uint32_t *holeSizeInWord)
{
    CellGcmContextData *ctx;
    (void)mode; (void)count; (void)type; (void)indices;
    (void)indexOffset;
    if (!g_record_mode) return 1;
    ctx = gCellGcmCurrentContext;
    if (!ctx) return 1;
    /* exact draw-word count deferred to SPU hole-patch path;
       stub returns error until real state validation wired */
    return 1;
}

PSGL_EXPORT int psglGenerateCommandBufferHole(uint32_t holeSizeInWord,
                                              uint32_t *holeEA)
{
    CellGcmContextData *ctx;
    uint32_t *hole_ptr;
    uint32_t i, total, aligned_pos;
    if (!g_record_mode) return 1;
    ctx = gCellGcmCurrentContext;
    if (!ctx || !holeEA || holeSizeInWord < 4u) return 1;
    total = holeSizeInWord + 31u;
    /* 128-byte (32-word) alignment */
    aligned_pos = ((uintptr_t)ctx->current & 31u) ? 1u : 0u;
    if (aligned_pos) {
        ctx->current[0] = 0u; /* NOP */
        ctx->current++;
    }
    hole_ptr = ctx->current;
    cellGcmAddressToOffset(hole_ptr, holeEA);
    psgl_cb_jump_to_self(ctx);
    ctx->current++;
    for (i = 1u; i < total; i++) {
        if ((i & 31u) == 0u)
            psgl_cb_jump_to_self(ctx);
        else
            ctx->current[0] = 0u;
        ctx->current++;
    }
    return 0;
}
