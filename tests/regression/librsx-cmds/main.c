/* librsx-cmds: words written by the PSL1GHT-style rsx command functions into
 * a plain buffer (no RSX involved), checked against values worked out here.
 * Built once against librsx and once against libgcm_cmd's legacy copy of the
 * same functions, in both ABIs.
 *
 * - rsxSetupContextData: context->end is the last word of the buffer;
 * - rsxSetVertexProgramConstants with 48 floats: one 32-float packet, both
 *   16-float halves from their own source, then a 16-float remainder copied
 *   as float bit patterns (not converted to integers);
 * - rsxSetTimeStamp: a two-word packet with the report index in word 1;
 * - PSL1GHT's vdecH264Info has CellVdecAvcInfo's size and member offsets;
 *
 * Prints LIBRSX_CMDS_OK, or a LIBRSX_CMDS_FAIL line per mismatch. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <rsx/rsx.h>
#include <codec/vdec.h>
#include <cell/codec/vdec_avc.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define VP_UPLOAD_CONST_ID 0x1efc
#define QUERY_GET          0x1800

static u32 buf[128] __attribute__((aligned(128)));
static gcmContextData ctx;
static int fails;

static void fail(const char *what, unsigned i, u32 got, u32 want)
{
	printf("LIBRSX_CMDS_FAIL %s: word %u = 0x%08x, want 0x%08x\n", what, i, (unsigned)got, (unsigned)want);
	fails++;
}

static u32 header(u32 method, u32 count) { return (count << 18) | method; }

static u32 bits(float f)
{
	u32 u;
	memcpy(&u, &f, sizeof u);
	return u;
}

static s32 no_wrap(gcmContextData *c, u32 count)
{
	(void)c;
	(void)count;
	printf("LIBRSX_CMDS_FAIL unexpected buffer wrap\n");
	fails++;
	return -1;
}

static void reset(void)
{
	memset(buf, 0, sizeof buf);
	rsxSetupContextData(&ctx, buf, sizeof buf, no_wrap);
	/* Setup emits its own DMA-destination packet; start each probe at the beginning. */
	memset(buf, 0, sizeof buf);
	ctx.current = ctx.begin;
}

int main(void)
{
	printf("librsx-cmds: sizeof(void*)=%u\n", (unsigned)sizeof(void *));

	reset();
	/* As a word index: the context's pointer fields are 32-bit under LP64. */
	{
		long end = (long)(ctx.end - buf), want = (long)(sizeof buf / 4) - 1;
		if (end != want) {
			printf("LIBRSX_CMDS_FAIL context end at word %ld, want %ld\n", end, want);
			fails++;
		}
	}
	if (ctx.begin != buf)
		fails++, printf("LIBRSX_CMDS_FAIL context begin\n");

	{
		float v[48];
		unsigned i;
		for (i = 0; i < 48; i++)
			v[i] = (float)i + 0.5f;
		reset();
		rsxSetVertexProgramConstants(&ctx, 10, 48, v);
		u32 want[2 + 32 + 2 + 16];
		want[0] = header(VP_UPLOAD_CONST_ID, 33);
		want[1] = 10;
		for (i = 0; i < 32; i++)
			want[2 + i] = bits(v[i]);
		want[34] = header(VP_UPLOAD_CONST_ID, 17);
		want[35] = 10 + 8;
		for (i = 0; i < 16; i++)
			want[36 + i] = bits(v[32 + i]);
		unsigned n = (unsigned)(ctx.current - buf);
		if (n != 52)
			fails++, printf("LIBRSX_CMDS_FAIL VertexProgramConstants wrote %u words, want 52\n", n);
		for (i = 0; i < 52; i++)
			if (buf[i] != want[i])
				fail("VertexProgramConstants", i, buf[i], want[i]);
	}

	reset();
	rsxSetTimeStamp(&ctx, 5);
	{
		const u32 want[2] = { header(QUERY_GET, 1), (5u << 4) | 0x10000000u };
		unsigned i, n = (unsigned)(ctx.current - buf);
		if (n != 2)
			fails++, printf("LIBRSX_CMDS_FAIL SetTimeStamp wrote %u words, want 2\n", n);
		for (i = 0; i < 3; i++)
			if (buf[i] != (i < 2 ? want[i] : 0))
				fail("SetTimeStamp", i, buf[i], i < 2 ? want[i] : 0);
	}

#ifndef LIBRSX_CMDS_SKIP_HEADER_CHECKS	/* lets a harness run the word checks against older headers */
	if (sizeof(vdecH264Info) != sizeof(CellVdecAvcInfo))
		fails++, printf("LIBRSX_CMDS_FAIL sizeof vdecH264Info %u, CellVdecAvcInfo %u\n",
		                (unsigned)sizeof(vdecH264Info), (unsigned)sizeof(CellVdecAvcInfo));
#define SAME_OFFSET(a, b) \
	if (offsetof(vdecH264Info, a) != offsetof(CellVdecAvcInfo, b)) \
		fails++, printf("LIBRSX_CMDS_FAIL offset of " #a " %u, " #b " %u\n", \
		                (unsigned)offsetof(vdecH264Info, a), (unsigned)offsetof(CellVdecAvcInfo, b))
	SAME_OFFSET(pic_order_count, picOrderCount);
	SAME_OFFSET(vui_parameters_present_flag, vui_parameters_present_flag);
	SAME_OFFSET(nalUnitPresentFlags, nalUnitPresentFlags);
	SAME_OFFSET(ccDataLength, ccDataLength);
	SAME_OFFSET(ccData, ccData);
	SAME_OFFSET(reserved, reserved);
	if (sizeof(((vdecH264Info *)0)->pic_order_count[0]) != 2)
		fails++, printf("LIBRSX_CMDS_FAIL pic_order_count is not 16-bit\n");

#endif

	if (fails)
		printf("LIBRSX_CMDS_FAIL %d\n", fails);
	else
		printf("LIBRSX_CMDS_OK\n");
	return fails != 0;
}
