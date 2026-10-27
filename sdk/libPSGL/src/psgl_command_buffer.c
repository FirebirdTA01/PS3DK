/* psgl_command_buffer.c -- Static Command Buffer implementation
 *
 * Implements the 10 psgl command-buffer APIs from PSGL/psgl.h.
 * Keeps FIFO recording state separate from GL draw-state (psgl_context.c).
 */
#include <PSGL/psgl.h>
#include <stdint.h>
#include <malloc.h>
#include <stdio.h>
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

/* Buffers from psglAllocateCommandBuffer: base address and mapped extent, so
   psglFreeCommandBuffer unmaps exactly what was mapped. One record per live
   buffer, allocated with it; there is no fixed limit. */
struct psgl_cb_buffer {
    struct psgl_cb_buffer *next;
    void *base;
    uint32_t size;
};
static struct psgl_cb_buffer *g_cb_buffers;

static struct psgl_cb_buffer **psgl_cb_find(const void *base)
{
    for (struct psgl_cb_buffer **p = &g_cb_buffers; *p; p = &(*p)->next)
        if ((*p)->base == base) return p;
    return NULL;
}

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
    struct psgl_cb_buffer *record = malloc(sizeof(*record));
    if (!record) return NULL;
    mem = memalign(1024u * 1024u, (size_t)rounded);
    if (!mem) {
        free(record);
        return NULL;
    }
    memset(mem, 0, (size_t)rounded);
    if (cellGcmMapMainMemory(mem, (size_t)rounded, offset) != 0) {
        free(mem);
        free(record);
        return NULL;
    }
    record->base = mem;
    record->size = rounded;
    record->next = g_cb_buffers;
    g_cb_buffers = record;
    return mem;
}

/* The GPU may still execute or CALL into the buffer, so it is unmapped and
   freed only after everything queued so far has completed. If the GPU does
   not drain within the teardown bound, or the unmap fails, the buffer stays
   mapped and allocated and GL_INVALID_OPERATION is recorded. */
PSGL_EXPORT void psglFreeCommandBuffer(void *commandBuffer)
{
    struct psgl_cb_buffer **link, *record;
    int32_t rc;
    if (!commandBuffer) return;
    link = psgl_cb_find(commandBuffer);
    if (!link) {
        fprintf(stderr, "PSGL: psglFreeCommandBuffer: %p was not allocated by "
                "psglAllocateCommandBuffer\n", commandBuffer);
        psgl_gl_error(GL_INVALID_VALUE);
        return;
    }
    if (!psgl_context_wait_idle_bounded(gCellGcmCurrentContext,
                                        "psglFreeCommandBuffer")) {
        psgl_gl_error(GL_INVALID_OPERATION);
        return;
    }
    record = *link;
    rc = cellGcmUnmapEaIoAddress(record->base);
    if (rc != 0) {
        fprintf(stderr, "PSGL: psglFreeCommandBuffer: unmapping %p (%u bytes) "
                "failed (%08x); keeping it\n", record->base,
                (unsigned)record->size, (unsigned)rc);
        psgl_gl_error(GL_INVALID_OPERATION);
        return;
    }
    *link = record->next;
    free(record);
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
    uint32_t words, chunks, rest;
    if (!g_record_mode || !holeEA || !holeSizeInWord || count <= 0)
        return 1;
    ctx = gCellGcmCurrentContext;
    if (!ctx) return 1;
    (void)mode; (void)type; (void)indices;
    if (indexOffset) *indexOffset = 0u;

    /* calculate draw-hole word count using the reference formula */
    words = 7u + 2u + 3u;
    chunks = (uint32_t)count / 0x7ff00u;
    words += chunks * (0x7ffu + 1u);
    rest = (uint32_t)count % 0x7ff00u;
    words += rest / 0x100u;
    if (rest % 0x100u) words++;
    words += words / 32u;
    words += 4u - (words % 4u);

    *holeSizeInWord = words;
    return psglGenerateCommandBufferHole(words, holeEA);
}

PSGL_EXPORT int psglGenerateCommandBufferHole(uint32_t holeSizeInWord,
                                              uint32_t *holeEA)
{
    CellGcmContextData *ctx;
    uint32_t *hole_ptr;
    uint32_t i, total;
    if (!g_record_mode) return 1;
    ctx = gCellGcmCurrentContext;
    if (!ctx || !holeEA || holeSizeInWord < 4u) return 1;
    if (ctx->current + holeSizeInWord + 31u > ctx->end) return 1;

    /* leading NOPs until 128-byte alignment (not counted in hole) */
    while ((uintptr_t)ctx->current & 127u) {
        ctx->current[0] = 0u;
        ctx->current++;
    }

    hole_ptr = ctx->current;
    cellGcmAddressToOffset(hole_ptr, holeEA);
    total = holeSizeInWord;
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
