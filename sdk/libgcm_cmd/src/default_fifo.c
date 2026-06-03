/*
 * PS3 Custom Toolchain — libgcm_cmd / default FIFO sizing
 *
 * The firmware default command buffer starts the first usable segment
 * after a 4 KiB initialization area.  `segmentSize` is specified in
 * words, so the first active segment has 1024 fewer words available.
 */

#include <rsx/rsx.h>

#define PS3TC_GCM_INIT_WORDS 1024u

static u32 g_ps3tc_default_command_words;
static u32 g_ps3tc_default_segment_words;

static int ps3tc_apply_default_fifo_words(u32 bufferSize, u32 segmentSize)
{
    if (!gGcmContext || !gGcmContext->begin)
        return -1;
    if (segmentSize <= PS3TC_GCM_INIT_WORDS)
        return -1;
    if (bufferSize < segmentSize * 2u)
        return -1;

    g_ps3tc_default_command_words = bufferSize;
    g_ps3tc_default_segment_words = segmentSize;

    gGcmContext->current = gGcmContext->begin;
    /* Use the WHOLE command buffer as the ring, not just one segment.
       A single-segment ring forces a wrap (and a GPU drain) several
       times per command-heavy frame, which serializes CPU and GPU and
       cannot coexist with in-stream GPU waits (semaphore-acquire) that
       legitimately block GET mid-segment.  Wrapping over the full
       buffer drops the wrap rate to roughly once per several frames and
       lets the GPU run concurrently with command production. */
    gGcmContext->end = gGcmContext->begin +
                       (bufferSize - PS3TC_GCM_INIT_WORDS) - 1u;
    return 0;
}

s32 gcmSetDefaultCommandBufferAndSegmentWordSize(const u32 bufferSize,
                                                 const u32 segmentSize)
{
    return (s32)ps3tc_apply_default_fifo_words(bufferSize, segmentSize);
}

u32 gcmGetDefaultCommandWordSize(void)
{
    return g_ps3tc_default_command_words;
}

u32 gcmGetDefaultSegmentWordSize(void)
{
    return g_ps3tc_default_segment_words;
}

s32 gcmSetDefaultFifoSize(const u32 bufferSize, const u32 segmentSize)
{
    return (s32)ps3tc_apply_default_fifo_words(bufferSize, segmentSize);
}
