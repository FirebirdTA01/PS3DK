/*
 * sprx_segments: a module that writes its own .data and .bss.
 *
 * On a module with one writable+executable segment, retail Lv-2 freezes the
 * process at the first of these stores; with the code/data split lv2-prx.ld
 * links, they must just work. module_start reports every value it saw
 * through argp, so the loader can print a verdict for each step instead of
 * the module returning a single pass/fail.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/prx.h>
#include <sys/moduleexport.h>

#include "segments_contract.h"

SYS_MODULE_INFO(sprx_segments, 0, 1, 0);
SYS_MODULE_START(segments_start);
SYS_MODULE_STOP(segments_stop);

/* volatile: every access below must be a real load or store. */
static volatile uint32_t g_data = SEG_DATA_INITIAL;   /* .data */
static volatile uint32_t g_bss;                        /* .bss  */
static volatile uint32_t g_bumps;                      /* .bss  */
static volatile uint32_t g_block[SEG_BSS_BLOCK_WORDS]; /* .bss  */

/* .rodata, reached TOC-relative from .text. */
static const int k_table[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

int seg_calc_add(int a, int b)
{
	return a + b + 0x42;
}

int seg_calc_scale(int x, int i)
{
	return x * k_table[i & 7];
}

uint32_t seg_state_data(void)
{
	return g_data;
}

uint32_t seg_state_bss(void)
{
	return g_bss;
}

uint32_t seg_state_bump(void)
{
	g_bumps = g_bumps + 1;
	return g_bumps;
}

int segments_start(size_t args, void *argp)
{
	seg_start_report_t *rep = NULL;
	if (args >= sizeof(seg_start_report_t) && argp != NULL)
		rep = (seg_start_report_t *)argp;

	uint32_t data_before = g_data;
	g_data = SEG_DATA_STARTED;
	uint32_t data_after = g_data;

	uint32_t bss_before = g_bss;
	g_bss = SEG_BSS_STARTED;
	uint32_t bss_after = g_bss;

	for (uint32_t i = 0; i < SEG_BSS_BLOCK_WORDS; i++)
		g_block[i] = 0xB10C0000u ^ (i * 0x01010101u);
	uint32_t bad = 0;
	for (uint32_t i = 0; i < SEG_BSS_BLOCK_WORDS; i++)
		if (g_block[i] != (0xB10C0000u ^ (i * 0x01010101u)))
			bad++;

	uint32_t sum = 0;
	for (int i = 0; i < 8; i++)
		sum += (uint32_t)seg_calc_scale(1, i);

	if (rep != NULL) {
		rep->data_before = data_before;
		rep->data_after = data_after;
		rep->bss_before = bss_before;
		rep->bss_after = bss_after;
		rep->block_bad = bad;
		rep->rodata_sum = sum;
		rep->magic = SEG_START_MAGIC;
	}
	return SYS_PRX_RESIDENT;
}

int segments_stop(size_t args, void *argp)
{
	uint32_t data_before = g_data;
	g_data = SEG_DATA_STOPPED;
	uint32_t data_after = g_data;

	if (args >= sizeof(seg_stop_report_t) && argp != NULL) {
		seg_stop_report_t *rep = (seg_stop_report_t *)argp;
		rep->data_before = data_before;
		rep->data_after = data_after;
		rep->bumps = g_bumps;
		rep->magic = SEG_STOP_MAGIC;
	}
	return SYS_PRX_STOP_OK;
}
