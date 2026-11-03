#!/usr/bin/env python3
"""Host unit test for cellGcmSetTransferData / rsxSetTransferData emitter.

Validates the NV_MEMORY_TO_MEMORY_FORMAT command emitter against production
headers (sdk/libgcm_cmd/src/rsx_legacy/commands_impl.h and sdk/librsx/src/commands_impl.h):
  - Proves byte-identity of SetTransferData implementations across both SDK copies.
  - Pins exact method words across 18 test cases (packed rows, padded pitches,
    all transfer modes, in-range negative pitches, boundary signed pitches
    [-32768, +32767], out-of-range pitches [32768, -32769, 40000, -40000],
    zero rows on both batched and fallback paths, column split at MAX_LINES
    and MAX_LINES + 1 with advancing offsets, and row chunking > MAX_ROWS).
  - Verifies negative controls:
    (1) Reservation exhaustion refusal on a single contiguous backing array with
        interior guard ranges: verifies clean return without out-of-bounds writes
        or guard canary corruption.
    (2) Canary corruption negative control: proves that check_guards detects
        adjacent guard corruption.
    (3) Watchdog control on restored historical broken loop: verifies that the
        restored broken loop flushes an entry/progress receipt, produces no
        sanitizer diagnostics, and did not return before the watchdog timer fired.
"""
import argparse
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--libgcm-cmd", type=Path,
                    default=ROOT / "sdk/libgcm_cmd/src/rsx_legacy/commands_impl.h")
parser.add_argument("--librsx", type=Path,
                    default=ROOT / "sdk/librsx/src/commands_impl.h")
parser.add_argument("--upstream-psl1ght", type=Path,
                    default=ROOT / "src/ps3dev/PSL1GHT/ppu/librsx/commands_impl.h")
parser.add_argument("--watchdog-timeout", type=float, default=1.0,
                    help="Watchdog timeout in seconds for infinite loop control")
args = parser.parse_args()

# Validate --watchdog-timeout is finite and positive
if not math.isfinite(args.watchdog_timeout) or args.watchdog_timeout <= 0:
    sys.exit(f"Error: --watchdog-timeout must be finite and positive, got {args.watchdog_timeout}")


def extract_function(signature, text):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[start:end]


# 1. Verify source existence and byte-identity across both SDK copies
if not args.libgcm_cmd.is_file():
    sys.exit(f"Missing SDK header: {args.libgcm_cmd}")
if not args.librsx.is_file():
    sys.exit(f"Missing SDK header: {args.librsx}")

src_gcm = args.libgcm_cmd.read_text(encoding="utf-8")
src_rsx = args.librsx.read_text(encoding="utf-8")

fn_internal_gcm = extract_function(
    "static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(SetTransferData)",
    src_gcm
)
fn_internal_rsx = extract_function(
    "static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(SetTransferData)",
    src_rsx
)
if fn_internal_gcm != fn_internal_rsx:
    sys.exit("RSX_FUNC_INTERNAL(SetTransferData) differs between libgcm_cmd and librsx")

fn_pub_gcm = extract_function(
    "void RSX_FUNC(SetTransferData)(gcmContextData *context,u8 mode",
    src_gcm
)
fn_pub_rsx = extract_function(
    "void RSX_FUNC(SetTransferData)(gcmContextData *context,u8 mode",
    src_rsx
)
if fn_pub_gcm != fn_pub_rsx:
    sys.exit("RSX_FUNC(SetTransferData) differs between libgcm_cmd and librsx")

# 2. Pinned historical defective fixture (never dynamically substituted)
PINNED_DEFECTIVE_INTERNAL = r'''
static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(SetTransferData)(gcmContextData *context,u32 dstOffset,u32 dstPitch,u32 srcOffset,u32 srcPitch,u32 bytesPerRow,u32 rowCount)
{
	const s32 MIN_PITCH = -32768;
	const s32 MAX_PITCH =  32768;
	const s32 MAX_ROWS = 0x7ff;
	const u32 MAX_LINES = 0x3fffff;
	u32 colCount, rows, cols;

	if (srcPitch == bytesPerRow && dstPitch == bytesPerRow) {
		bytesPerRow *= rowCount;
		rowCount = 1;
		srcPitch = 0;
		dstPitch = 0;
	}

	if (srcPitch < MIN_PITCH || srcPitch > MAX_PITCH || dstPitch < MIN_PITCH || dstPitch > MAX_PITCH) {
		while (--rowCount >= 0) {
			for (colCount=bytesPerRow;colCount > 0;colCount-=cols) {
				cols = (colCount > MAX_LINES) ? MAX_LINES : colCount;

				RSX_CONTEXT_CURRENT_BEGIN(9);
				RSX_CONTEXT_CURRENTP[0] = RSX_SUBCHANNEL_METHOD(1,NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN,8);
				RSX_CONTEXT_CURRENTP[1] = srcOffset + (bytesPerRow - colCount);
				RSX_CONTEXT_CURRENTP[2] = dstOffset + (bytesPerRow - colCount);
				RSX_CONTEXT_CURRENTP[3] = 0;
				RSX_CONTEXT_CURRENTP[4] = 0;
				RSX_CONTEXT_CURRENTP[5] = cols;
				RSX_CONTEXT_CURRENTP[6] = 1;
				RSX_CONTEXT_CURRENTP[7] = (((u32)1<<8) | 1);
				RSX_CONTEXT_CURRENTP[8] = 0;
				RSX_CONTEXT_CURRENT_END(9);
			}
		}
	} else {
		for (;rowCount > 0;rowCount-=rows) {
			rows = (rowCount > MAX_ROWS) ? MAX_ROWS : rowCount;

			for (colCount=bytesPerRow;colCount > 0;colCount-=cols) {
				cols = (colCount > MAX_LINES) ? MAX_LINES : colCount;

				RSX_CONTEXT_CURRENT_BEGIN(9);
				RSX_CONTEXT_CURRENTP[0] = RSX_SUBCHANNEL_METHOD(1,NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN,8);
				RSX_CONTEXT_CURRENTP[1] = srcOffset + (bytesPerRow - colCount);
				RSX_CONTEXT_CURRENTP[2] = dstOffset + (bytesPerRow - colCount);
				RSX_CONTEXT_CURRENTP[3] = srcPitch;
				RSX_CONTEXT_CURRENTP[4] = dstPitch;
				RSX_CONTEXT_CURRENTP[5] = cols;
				RSX_CONTEXT_CURRENTP[6] = rows;
				RSX_CONTEXT_CURRENTP[7] = (((u32)1<<8) | 1);
				RSX_CONTEXT_CURRENTP[8] = 0;
				RSX_CONTEXT_CURRENT_END(9);
			}

			srcOffset += (rows*srcPitch);
			dstOffset += (rows*dstPitch);
		}
	}
}
'''

PINNED_DEFECTIVE_PUB = r'''
void RSX_FUNC(SetTransferData)(gcmContextData *context,u8 mode,u32 dst,u32 outpitch,u32 src,u32 inpitch,u32 linelength,u32 linecount)
{
	RSX_CONTEXT_CURRENT_BEGIN(3);
	RSX_CONTEXT_CURRENTP[0] = RSX_SUBCHANNEL_METHOD(1,NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN,2);
	RSX_CONTEXT_CURRENTP[1] = (mode&0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
	RSX_CONTEXT_CURRENTP[2] = (mode&0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
	RSX_CONTEXT_CURRENT_END(3);

	RSX_FUNC_INTERNAL(SetTransferData)(context, dst, outpitch, src, inpitch, linelength, linecount);
}
'''

# 3. Optional upstream PSL1GHT verification
if args.upstream_psl1ght.is_file():
    src_psl1ght = args.upstream_psl1ght.read_text(encoding="utf-8")
    if "RSX_FUNC_INTERNAL(SetTransferData)" not in src_psl1ght:
        sys.exit(f"FAIL: {args.upstream_psl1ght} exists but lacks RSX_FUNC_INTERNAL(SetTransferData)!")
    upstream_fn = extract_function(
        "static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(SetTransferData)",
        src_psl1ght
    )
    norm_upstream = re.sub(r"\s+", " ", upstream_fn).strip()
    norm_pinned = re.sub(r"\s+", " ", PINNED_DEFECTIVE_INTERNAL).strip()
    if norm_upstream != norm_pinned:
        sys.exit(f"FAIL: {args.upstream_psl1ght} does not match pinned historical defective fixture!")
    print("upstream-psl1ght check: verified (matches pinned historical defective baseline)")
else:
    print("upstream-psl1ght check: skipped (file not present)")

C_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef int32_t s32;
typedef uint32_t u32;

typedef struct _gcmCtxData {
    uint32_t *begin;
    uint32_t *end;
    uint32_t *current;
    int32_t (*callback)(struct _gcmCtxData *, uint32_t);
} gcmContextData;

#define GCM_DMA_MEMORY_FRAME_BUFFER 0xFEED0000u
#define GCM_DMA_MEMORY_HOST_BUFFER  0xFEED0001u

#define NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN 0x00000184u
#define NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN     0x0000030cu

#define RSX_METHOD_COUNT_SHIFT 18
#define RSX_SUBCHANNEL_SHIFT   13
#define RSX_SUBCHANNEL_METHOD(channel, method, count) \
    (((uint32_t)(count) << RSX_METHOD_COUNT_SHIFT) | ((uint32_t)(channel) << RSX_SUBCHANNEL_SHIFT) | (method))

#define RSX_CONTEXT_CURRENTP (context->current)
#define RSX_CONTEXT_CURRENT_END(x) do { context->current += (x); } while(0)

/*
 * Memory-safe reservation shim:
 * Compares remaining capacity without forming any out-of-bounds pointer.
 * Revalidates remaining capacity after callback before writing.
 */
#define RSX_CONTEXT_CURRENT_BEGIN(count) do { \
    size_t rem = (context->current <= context->end) ? (size_t)(context->end - context->current) : 0; \
    if ((size_t)(count) > rem) { \
        if (context->callback && context->callback(context, (count)) != 0) return; \
        rem = (context->current <= context->end) ? (size_t)(context->end - context->current) : 0; \
        if ((size_t)(count) > rem) return; \
    } \
} while(0)

#define RSX_FUNC_INTERNAL(name) fixed_internal_##name
#define RSX_FUNC(name) fixed_##name

/* --- Production fixed implementation extracted from SDK --- */
''' + fn_internal_gcm + '\n' + fn_pub_gcm + r'''
#undef RSX_FUNC_INTERNAL
#undef RSX_FUNC

/* --- Historical defective implementation --- */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#define RSX_FUNC_INTERNAL(name) old_internal_##name
#define RSX_FUNC(name) old_##name
''' + PINNED_DEFECTIVE_INTERNAL + '\n' + PINNED_DEFECTIVE_PUB + r'''
#undef RSX_FUNC_INTERNAL
#undef RSX_FUNC

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define MAX_WORDS 4096
static uint32_t s_buf[MAX_WORDS];
static gcmContextData s_ctx;

static void reset_context(void)
{
    memset(s_buf, 0xAA, sizeof(s_buf));
    s_ctx.begin = s_buf;
    s_ctx.current = s_buf;
    s_ctx.end = s_buf + MAX_WORDS;
    s_ctx.callback = NULL;
}

static int expect_words(const char *name, const uint32_t *expected, uint32_t count)
{
    uint32_t written = (uint32_t)(s_ctx.current - s_ctx.begin);
    if (written != count) {
        fprintf(stderr, "FAIL %s: written=%u expected=%u\n", name, written, count);
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (s_buf[i] != expected[i]) {
            fprintf(stderr, "FAIL %s word %u: got 0x%08x expected 0x%08x\n",
                    name, i, s_buf[i], expected[i]);
            return 0;
        }
    }
    return 1;
}

#define SUB(m, n) (((uint32_t)(n) << 18) | (1u << 13) | (m))
#define DMA(in_h, out_h) SUB(0x184, 2), ((in_h) ? 0xFEED0001u : 0xFEED0000u), ((out_h) ? 0xFEED0001u : 0xFEED0000u)
#define RUN(src, dst, sp, dp, cols, rows) \
    SUB(0x30c, 8), (src), (dst), (uint32_t)(sp), (uint32_t)(dp), (cols), (rows), 0x101u, 0u

static int run_fixed_tests(void)
{
    int pass = 0, total = 0;

    /* Case 1: Packed rows (mode=0, pitch == bytesPerRow).
     * Compresses to 1 line of length bytesPerRow * rowCount. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 16, 0x2000, 16, 16, 4);
    {
        const uint32_t want[] = { DMA(0, 0), RUN(0x2000, 0x1000, 0, 0, 64, 1) };
        pass += expect_words("packed rows", want, 12);
    }

    /* Case 2: Padded pitches within field range (mode=0). */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 32, 16, 3);
    {
        const uint32_t want[] = { DMA(0, 0), RUN(0x2000, 0x1000, 32, 64, 16, 3) };
        pass += expect_words("padded pitches", want, 12);
    }

    /* Case 3: Mode LOCAL_TO_MAIN (mode=2, bit 1 set). */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 2, 0x1000, 64, 0x2000, 32, 16, 3);
    {
        const uint32_t want[] = { DMA(0, 1), RUN(0x2000, 0x1000, 32, 64, 16, 3) };
        pass += expect_words("mode LOCAL_TO_MAIN", want, 12);
    }

    /* Case 4: Mode MAIN_TO_LOCAL (mode=1, bit 0 set). */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 1, 0x1000, 64, 0x2000, 32, 16, 3);
    {
        const uint32_t want[] = { DMA(1, 0), RUN(0x2000, 0x1000, 32, 64, 16, 3) };
        pass += expect_words("mode MAIN_TO_LOCAL", want, 12);
    }

    /* Case 5: Mode MAIN_TO_MAIN (mode=3, bits 0 and 1 set). */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 3, 0x1000, 64, 0x2000, 32, 16, 3);
    {
        const uint32_t want[] = { DMA(1, 1), RUN(0x2000, 0x1000, 32, 64, 16, 3) };
        pass += expect_words("mode MAIN_TO_MAIN", want, 12);
    }

    /* Case 6: Negative destination pitch (in-range signed pass-through). */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, (uint32_t)-64, 0x2000, 32, 16, 2);
    {
        const uint32_t want[] = { DMA(0, 0), RUN(0x2000, 0x1000, 32, -64, 16, 2) };
        pass += expect_words("negative dst pitch", want, 12);
    }

    /* Case 7: Negative source pitch (in-range signed pass-through). */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, (uint32_t)-32, 16, 2);
    {
        const uint32_t want[] = { DMA(0, 0), RUN(0x2000, 0x1000, -32, 64, 16, 2) };
        pass += expect_words("negative src pitch", want, 12);
    }

    /* Case 8: IN-range signed boundary MIN_PITCH (-32768). Batched path. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, (uint32_t)-32768, 16, 2);
    {
        const uint32_t want[] = { DMA(0, 0), RUN(0x2000, 0x1000, -32768, 64, 16, 2) };
        pass += expect_words("in-range MIN_PITCH (-32768)", want, 12);
    }

    /* Case 9: IN-range signed boundary MAX_PITCH (+32767). Batched path. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 32767, 16, 2);
    {
        const uint32_t want[] = { DMA(0, 0), RUN(0x2000, 0x1000, 32767, 64, 16, 2) };
        pass += expect_words("in-range MAX_PITCH (+32767)", want, 12);
    }

    /* Case 10: Out-of-range signed boundary MAX_PITCH + 1 (32768). Fallback path with advancing offsets. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 32768, 16, 2);
    {
        const uint32_t want[] = {
            DMA(0, 0),
            RUN(0x2000, 0x1000, 0, 0, 16, 1),
            RUN(0x2000 + 32768, 0x1000 + 64, 0, 0, 16, 1)
        };
        pass += expect_words("boundary pitch 32768", want, 21);
    }

    /* Case 11: Out-of-range signed boundary MIN_PITCH - 1 (-32769). Fallback path with advancing offsets. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, (uint32_t)-32769, 16, 2);
    {
        const uint32_t want[] = {
            DMA(0, 0),
            RUN(0x2000, 0x1000, 0, 0, 16, 1),
            RUN(0x2000 + (uint32_t)-32769, 0x1000 + 64, 0, 0, 16, 1)
        };
        pass += expect_words("boundary pitch -32769", want, 21);
    }

    /* Case 12: Arbitrary wide pitch (40000 > 32767). Fallback path with advancing offsets. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 16, 0x2000, 40000, 16, 2);
    {
        const uint32_t want[] = {
            DMA(0, 0),
            RUN(0x2000, 0x1000, 0, 0, 16, 1),
            RUN(0x2000 + 40000, 0x1000 + 16, 0, 0, 16, 1)
        };
        pass += expect_words("wide pitch 40000", want, 21);
    }

    /* Case 13: Arbitrary degenerate negative pitch (-40000 < -32768). Fallback path with advancing offsets. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 16, 0x2000, (uint32_t)-40000, 16, 2);
    {
        const uint32_t want[] = {
            DMA(0, 0),
            RUN(0x2000, 0x1000, 0, 0, 16, 1),
            RUN(0x2000 + (uint32_t)-40000, 0x1000 + 16, 0, 0, 16, 1)
        };
        pass += expect_words("degenerate negative pitch -40000", want, 21);
    }

    /* Case 14: Zero rows on batched path (rowCount = 0): DMA only. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 32, 16, 0);
    {
        const uint32_t want[] = { DMA(0, 0) };
        pass += expect_words("zero rows batched path", want, 3);
    }

    /* Case 15: Zero rows on fallback path (rowCount = 0, pitch out of range): DMA only. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 40000, 16, 0);
    {
        const uint32_t want[] = { DMA(0, 0) };
        pass += expect_words("zero rows fallback path", want, 3);
    }

    /* Case 16: Column split boundary at MAX_LINES (0x3fffff = 4194303). 1 run. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 32, 0x3fffff, 1);
    {
        const uint32_t want[] = { DMA(0, 0), RUN(0x2000, 0x1000, 32, 64, 0x3fffff, 1) };
        pass += expect_words("column boundary MAX_LINES", want, 12);
    }

    /* Case 17: Column split at MAX_LINES + 1 (0x400000 = 4194304). 2 runs with advancing column offsets. */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 32, 0x400000, 1);
    {
        const uint32_t want[] = {
            DMA(0, 0),
            RUN(0x2000, 0x1000, 32, 64, 0x3fffff, 1),
            RUN(0x2000 + 0x3fffff, 0x1000 + 0x3fffff, 32, 64, 1, 1)
        };
        pass += expect_words("column split MAX_LINES+1", want, 21);
    }

    /* Case 18: rowCount chunking (> MAX_ROWS 0x7ff = 2047). */
    total++;
    reset_context();
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 64, 0x2000, 32, 16, 2048);
    {
        const uint32_t want[] = {
            DMA(0, 0),
            RUN(0x2000, 0x1000, 32, 64, 16, 2047),
            RUN(0x2000 + 2047 * 32, 0x1000 + 2047 * 64, 32, 64, 16, 1)
        };
        pass += expect_words("row chunking 2048 rows", want, 21);
    }

    return pass == total ? 0 : 1;
}

/* Single contiguous backing array with interior capacity and guard ranges */
#define GUARD_PATTERN 0xDEADBEEFu
#define GUARD_WORDS 16
#define TEST_CAPACITY 12 /* Exactly enough for DMA (3 words) + 1 run (9 words) = 12 words */
#define TOTAL_WORDS (GUARD_WORDS + TEST_CAPACITY + GUARD_WORDS)

static uint32_t s_single_backing[TOTAL_WORDS];
#define S_GUARD_BEFORE (&s_single_backing[0])
#define S_BOUNDED_BUF  (&s_single_backing[GUARD_WORDS])
#define S_GUARD_AFTER  (&s_single_backing[GUARD_WORDS + TEST_CAPACITY])

static uint32_t g_exhaustion_calls = 0;
static uint32_t g_last_exhaustion_count = 0;

static int32_t refuse_exhaustion_callback(gcmContextData *context, uint32_t count)
{
    (void)context;
    g_exhaustion_calls++;
    g_last_exhaustion_count = count;
    return -1; /* Refuse reservation immediately */
}

static void init_guards(void)
{
    for (size_t i = 0; i < GUARD_WORDS; i++) {
        S_GUARD_BEFORE[i] = GUARD_PATTERN;
        S_GUARD_AFTER[i]  = GUARD_PATTERN;
    }
    memset(S_BOUNDED_BUF, 0xAA, TEST_CAPACITY * sizeof(uint32_t));
}

static int check_guards(void)
{
    for (size_t i = 0; i < GUARD_WORDS; i++) {
        if (S_GUARD_BEFORE[i] != GUARD_PATTERN) {
            fprintf(stderr, "FAIL: leading guard canary [%zu] corrupted: 0x%08x\n", i, S_GUARD_BEFORE[i]);
            return 0;
        }
        if (S_GUARD_AFTER[i] != GUARD_PATTERN) {
            fprintf(stderr, "FAIL: trailing guard canary [%zu] corrupted: 0x%08x\n", i, S_GUARD_AFTER[i]);
            return 0;
        }
    }
    return 1;
}

static int run_reservation_exhaustion_test(void)
{
    init_guards();

    s_ctx.begin = S_BOUNDED_BUF;
    s_ctx.current = S_BOUNDED_BUF;
    s_ctx.end = S_BOUNDED_BUF + TEST_CAPACITY;
    s_ctx.callback = refuse_exhaustion_callback;
    g_exhaustion_calls = 0;
    g_last_exhaustion_count = 0;

    /* Request 2 runs: DMA (3 words) + Run 1 (9 words) + Run 2 (9 words) = 21 words.
     * With TEST_CAPACITY = 12, Run 2 exhausts the buffer and triggers callback. */
    fixed_SetTransferData(&s_ctx, 0, 0x1000, 16, 0x2000, 40000, 16, 2);

    if (g_exhaustion_calls != 1) {
        fprintf(stderr, "FAIL exhaustion: expected 1 call, got %u\n", g_exhaustion_calls);
        return 1;
    }
    if (g_last_exhaustion_count != 9) {
        fprintf(stderr, "FAIL exhaustion: expected requested count 9, got %u\n", g_last_exhaustion_count);
        return 1;
    }
    if (s_ctx.current != S_BOUNDED_BUF + TEST_CAPACITY) {
        fprintf(stderr, "FAIL exhaustion: current pointer %p != expected %p\n",
                (void*)s_ctx.current, (void*)(S_BOUNDED_BUF + TEST_CAPACITY));
        return 1;
    }
    if (!check_guards()) {
        return 1;
    }
    return 0;
}

/* Canary corruption control proving that check_guards reliably catches guard corruption */
static int run_canary_assertion_control(void)
{
    init_guards();
    /* Corrupt the adjacent trailing guard word */
    S_GUARD_AFTER[0] = 0xBAD0CAFEu;
    if (check_guards() == 0) {
        return 0; /* Detection successful, negative control passes */
    }
    fprintf(stderr, "FAIL: check_guards failed to detect corrupted trailing canary!\n");
    return 1;
}

#define RING_WORDS 256
static uint32_t s_ring[RING_WORDS];

static int32_t ring_buffer_callback(gcmContextData *context, uint32_t count)
{
    (void)count;
    /* Wrap ring buffer back to begin safely within valid bounds */
    context->current = context->begin;
    return 0;
}

static int run_broken_loop_control(void)
{
    for (size_t i = 0; i < RING_WORDS; i++) {
        s_ring[i] = 0;
    }
    s_ctx.begin = s_ring;
    s_ctx.current = s_ring;
    s_ctx.end = s_ring + RING_WORDS;
    s_ctx.callback = ring_buffer_callback;

    /* Emit explicit flushed progress receipt proving entry into emitter */
    printf("[broken-loop: entered and progress registered]\n");
    fflush(stdout);

    /* Old defective code:
     * When executed on positive workload (Case 1: packed rows), the combination
     * of unsigned pitch comparison against signed MIN_PITCH and while(--rowCount >= 0)
     * creates an infinite loop. Ring buffer wrapping ensures memory safety while looping. */
    old_SetTransferData(&s_ctx, 0, 0x1000, 16, 0x2000, 16, 16, 4);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--fixed") == 0) {
        return run_fixed_tests();
    }
    if (strcmp(argv[1], "--reservation-exhaustion") == 0) {
        return run_reservation_exhaustion_test();
    }
    if (strcmp(argv[1], "--canary-corruption-control") == 0) {
        return run_canary_assertion_control();
    }
    if (strcmp(argv[1], "--broken-loop-control") == 0) {
        return run_broken_loop_control();
    }
    fprintf(stderr, "Unknown mode: %s\n", argv[1]);
    return 2;
}
'''

cc_env = os.environ.get("CC")
if cc_env:
    cc = shlex.split(cc_env)
elif shutil.which("cc"):
    cc = ["cc"]
elif shutil.which("gcc"):
    cc = ["gcc"]
elif shutil.which("clang"):
    cc = ["clang"]
else:
    cc = ["cc"]


def check_sanitizer_support(cc_cmd):
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as tf:
        tf.write("int main(void){return 0;}\n")
        tf_name = tf.name
    out_bin = tf_name + (".exe" if os.name == "nt" else ".out")
    try:
        cmd = cc_cmd + ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", tf_name, "-o", out_bin]
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return res.returncode == 0
    except Exception:
        return False
    finally:
        for p in [tf_name, out_bin]:
            if os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass


# Sanitizers are required for this CI host gate; a failure to support is a test failure.
if not check_sanitizer_support(cc):
    sys.exit(f"FAIL: required non-recovering ASan/UBSan (-fsanitize=address,undefined -fno-sanitize-recover=all) not supported by compiler: {cc}")

san_flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]

with tempfile.TemporaryDirectory(prefix="gcm-transfer-") as tmpdir:
    tmp = Path(tmpdir)
    c_file = tmp / "test_harness.c"
    exe_file = tmp / ("test_harness.exe" if os.name == "nt" else "test_harness")
    c_file.write_text(C_HARNESS, encoding="utf-8")

    compile_flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-function", "-Wno-unused-parameter"] + san_flags

    cmd = cc + compile_flags + [str(c_file), "-o", str(exe_file)]
    print(f"compiler: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)

    passed = 0
    total = 21  # 18 fixed positive cases + 1 exhaustion control + 1 canary control + 1 broken loop watchdog control

    # 1. Run 18 fixed positive cases under watchdog
    try:
        res_fixed = subprocess.run([str(exe_file), "--fixed"],
                                   capture_output=True, text=True,
                                   timeout=args.watchdog_timeout)
        if res_fixed.returncode == 0:
            if any(w in res_fixed.stderr for w in ["Sanitizer", "runtime error:"]):
                print(f"FAIL fixed tests: sanitizer report detected:\n{res_fixed.stderr}", file=sys.stderr)
            else:
                passed += 18
        else:
            print(f"FAIL fixed tests rc={res_fixed.returncode}", file=sys.stderr)
            if res_fixed.stderr:
                print(res_fixed.stderr, file=sys.stderr)
    except subprocess.TimeoutExpired:
        print(f"FAIL fixed tests: timed out after {args.watchdog_timeout}s watchdog!", file=sys.stderr)

    # 2. Run reservation exhaustion control under watchdog
    try:
        res_exhaustion = subprocess.run([str(exe_file), "--reservation-exhaustion"],
                                        capture_output=True, text=True,
                                        timeout=args.watchdog_timeout)
        if res_exhaustion.returncode == 0:
            if any(w in res_exhaustion.stderr for w in ["Sanitizer", "runtime error:"]):
                print(f"FAIL exhaustion control: sanitizer report detected:\n{res_exhaustion.stderr}", file=sys.stderr)
            else:
                passed += 1
        else:
            print(f"FAIL exhaustion control rc={res_exhaustion.returncode}", file=sys.stderr)
            if res_exhaustion.stderr:
                print(res_exhaustion.stderr, file=sys.stderr)
    except subprocess.TimeoutExpired:
        print(f"FAIL exhaustion control: timed out after {args.watchdog_timeout}s watchdog!", file=sys.stderr)

    # 3. Run canary corruption control under watchdog
    try:
        res_canary = subprocess.run([str(exe_file), "--canary-corruption-control"],
                                    capture_output=True, text=True,
                                    timeout=args.watchdog_timeout)
        if res_canary.returncode == 0:
            passed += 1
        else:
            print(f"FAIL canary corruption control rc={res_canary.returncode}", file=sys.stderr)
    except subprocess.TimeoutExpired:
        print(f"FAIL canary corruption control: timed out after {args.watchdog_timeout}s watchdog!", file=sys.stderr)

    # 4. Classifier controls for broken-loop timeout verification
    def classify_watchdog_timeout(stdout_text, stderr_text):
        if "[broken-loop: entered and progress registered]" not in stdout_text:
            return False, "missing progress receipt (startup stall)"
        if any(w in stderr_text for w in ["Sanitizer", "runtime error:"]):
            return False, "sanitizer diagnostic detected"
        return True, "did not return before the watchdog"

    # Self-test classifier unit assertions
    ok_prog, _ = classify_watchdog_timeout("some output without token", "")
    assert not ok_prog, "classifier should reject missing progress receipt"
    ok_san, _ = classify_watchdog_timeout("[broken-loop: entered and progress registered]", "AddressSanitizer error")
    assert not ok_san, "classifier should reject sanitizer diagnostic"

    # 5. Run broken loop control under watchdog (must timeout / hang with verified progress)
    try:
        res_hang = subprocess.run([str(exe_file), "--broken-loop-control"],
                                  capture_output=True, text=True,
                                  timeout=args.watchdog_timeout)
        print(f"FAIL: broken loop completed unexpectedly with rc={res_hang.returncode} instead of timing out under watchdog!",
              file=sys.stderr)
    except subprocess.TimeoutExpired as exc:
        stdout_txt = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout.decode("utf-8", errors="replace") if exc.stdout else "")
        stderr_txt = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr.decode("utf-8", errors="replace") if exc.stderr else "")
        ok, reason = classify_watchdog_timeout(stdout_txt, stderr_txt)
        if ok:
            passed += 1
            print(f"broken-loop watchdog control: {reason}")
        else:
            print(f"FAIL broken-loop watchdog control: {reason}\nstdout: {stdout_txt}\nstderr: {stderr_txt}", file=sys.stderr)

    print(f"gcm-transfer-data: tests={total} pass={passed} fail={total-passed}")
    sys.exit(0 if passed == total else 1)
