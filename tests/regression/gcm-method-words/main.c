/* gcm-method-words: the command words the gcm texture and reserve functions
 * write, checked against values worked out here from the method numbers
 * and field layouts, into a plain buffer (no RSX involved).
 *
 * Prints GCM_WORDS_OK, or a GCM_WORDS_FAIL line per mismatch. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <cell/gcm.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static uint32_t buf[64];
static CellGcmContextData ctx;
static int fails;

/* method header: word count in bits 18-28, method address below */
static uint32_t header(uint32_t method, uint32_t count)
{
	return (count << 18) | method;
}

static void reset(void)
{
	memset(buf, 0, sizeof(buf));
	ctx.begin = ctx.current = buf;
	ctx.end = buf + 64;
	ctx.callback = 0;
}

static void expect(const char *what, const uint32_t *want, unsigned n)
{
	unsigned i, got = (unsigned)(ctx.current - buf);
	if (got != n) {
		printf("GCM_WORDS_FAIL %s: %u words, want %u\n", what, got, n);
		fails++;
		return;
	}
	for (i = 0; i < n; i++)
		if (buf[i] != want[i]) {
			printf("GCM_WORDS_FAIL %s: word %u = 0x%08x, want 0x%08x\n", what, i,
			       (unsigned)buf[i], (unsigned)want[i]);
			fails++;
		}
}

int main(void)
{
	/* texture unit 3: wrap s=1 t=2 r=3, unsigned remap 1, zfunc 4, gamma 5,
	 * aniso bias 6; address word 0x1a08 + 3 * 0x20 */
	reset();
	cellGcmSetTextureAddressAnisoBias(&ctx, 3, 1, 2, 3, 1, 4, 5, 6);
	{
		const uint32_t w[] = { header(0x1a08 + 3 * 0x20, 1),
		                       1u | 6u << 4 | 2u << 8 | 1u << 12 | 3u << 16 | 5u << 20 | 4u << 28 };
		expect("TextureAddressAnisoBias", w, 2);
	}

	/* texture unit 2: slope 9, iso 1, aniso 1; control2 at 0x0b00 + 2 * 4 */
	reset();
	cellGcmSetTextureOptimization(&ctx, 2, 9, 1, 1);
	{
		const uint32_t w[] = { header(0x0b00 + 2 * 4, 1), 9u | 1u << 6 | 1u << 7 | 0x2du << 8 };
		expect("TextureOptimization", w, 2);
	}

	/* vertex texture unit 1: X32 float, linear, normalized, 2D, one level */
	reset();
	{
		CellGcmTexture t;
		memset(&t, 0, sizeof(t));
		t.format = CELL_GCM_TEXTURE_X32_FLOAT | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
		t.mipmap = 1;
		t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
		t.width = 64;
		t.height = 32;
		t.location = CELL_GCM_LOCATION_LOCAL;
		t.pitch = 256;
		t.offset = 0x100080;
		cellGcmSetVertexTexture(&ctx, 1, &t);
		const uint32_t w[] = {
			header(0x0900 + 32, 2), 0x100080,
			(CELL_GCM_LOCATION_LOCAL + 1u) | (uint32_t)CELL_GCM_TEXTURE_DIMENSION_2 << 4 |
				(uint32_t)t.format << 8 | 1u << 16,
			header(0x0910 + 32, 1), 256,
			header(0x0918 + 32, 1), 32u | 64u << 16 };
		expect("VertexTexture", w, 7);
	}

	/* memory-to-memory transfers (subchannel 1): the DMA pair, then one
	 * OFFSET_IN..BUFFER_NOTIFY run of 8 per copy.  The pitches are signed and
	 * the fields hold -32768..32767; a pitch outside that is copied one row
	 * per run with both offsets advanced by their pitch.  Before this was
	 * fixed every ordinary pitch took the row path and its u32 row count
	 * never ran out, overrunning the buffer (EMP render-parity report). */
	{
		const uint32_t fb = 0xFEED0000u;
#define SUB(m, n) (((uint32_t)(n) << 18) | (1u << 13) | (m))
#define DMA SUB(0x184, 2), fb, fb
#define RUN(src, dst, sp, dp, cols, rows) \
		SUB(0x30c, 8), (src), (dst), (uint32_t)(sp), (uint32_t)(dp), (cols), (rows), 0x101u, 0u

		/* packed rows (pitch == bytes per row) collapse to one 64-byte line */
		reset();
		cellGcmSetTransferData(&ctx, CELL_GCM_TRANSFER_LOCAL_TO_LOCAL, 0x1000, 16, 0x2000, 16, 16, 4);
		{
			const uint32_t w[] = { DMA, RUN(0x2000, 0x1000, 0, 0, 64, 1) };
			expect("TransferData packed", w, 12);
		}

		/* padded pitches inside the field range: one run, three rows */
		reset();
		cellGcmSetTransferData(&ctx, CELL_GCM_TRANSFER_LOCAL_TO_LOCAL, 0x1000, 64, 0x2000, 32, 16, 3);
		{
			const uint32_t w[] = { DMA, RUN(0x2000, 0x1000, 32, 64, 16, 3) };
			expect("TransferData padded", w, 12);
		}

		/* a negative destination pitch is in range and passes through */
		reset();
		cellGcmSetTransferData(&ctx, CELL_GCM_TRANSFER_LOCAL_TO_LOCAL, 0x1000, (uint32_t)-64, 0x2000, 32, 16, 2);
		{
			const uint32_t w[] = { DMA, RUN(0x2000, 0x1000, 32, -64, 16, 2) };
			expect("TransferData negative pitch", w, 12);
		}

		/* a pitch the field cannot hold: one run per row, offsets advancing */
		reset();
		cellGcmSetTransferData(&ctx, CELL_GCM_TRANSFER_LOCAL_TO_LOCAL, 0x1000, 16, 0x2000, 40000, 16, 2);
		{
			const uint32_t w[] = { DMA, RUN(0x2000, 0x1000, 0, 0, 16, 1),
			                       RUN(0x2000 + 40000, 0x1000 + 16, 0, 0, 16, 1) };
			expect("TransferData wide pitch", w, 21);
		}

		/* 32768 is one past the field's maximum (32767) */
		reset();
		cellGcmSetTransferData(&ctx, CELL_GCM_TRANSFER_LOCAL_TO_LOCAL, 0x1000, 32768, 0x2000, 16, 16, 2);
		{
			const uint32_t w[] = { DMA, RUN(0x2000, 0x1000, 0, 0, 16, 1),
			                       RUN(0x2000 + 16, 0x1000 + 32768, 0, 0, 16, 1) };
			expect("TransferData pitch 32768", w, 21);
		}

		/* no rows: the DMA pair only */
		reset();
		cellGcmSetTransferData(&ctx, CELL_GCM_TRANSFER_LOCAL_TO_LOCAL, 0x1000, 64, 0x2000, 32, 16, 0);
		{
			const uint32_t w[] = { DMA };
			expect("TransferData no rows", w, 3);
		}
#undef RUN
#undef DMA
#undef SUB
	}

	/* a reserve that fits writes nothing and moves nothing */
	reset();
	cellGcmReserveMethodSize(&ctx, 16);
	expect("ReserveMethodSize", 0, 0);

	printf(fails ? "GCM_WORDS_FAIL %d\n" : "GCM_WORDS_OK\n", fails);
	return fails != 0;
}
