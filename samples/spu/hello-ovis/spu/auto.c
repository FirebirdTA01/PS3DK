/* hello-ovis, automatic mode: every function of the overlay objects is
 * wrapped, so a plain call maps its section first.  All four objects share
 * one LS range here.
 *
 * argument 1: overlay table EA, argument 2: report EA. */
#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <cell/ovis.h>
#include "ops.h"

#define TAG 5

static ovis_report_t report;

int main(uint64_t table, uint64_t report_ea)
{
	int i;
	if (cellOvisInitializeAutoMapping(table, TAG) != CELL_OK)
		sys_spu_thread_exit(1);
	for (i = 0; i < 2; i++) {
		report.results[4 * i + 0] = op_add(40, 2);
		report.results[4 * i + 1] = op_mul(6, 7);
		report.results[4 * i + 2] = op_fib(20);
		report.results[4 * i + 3] = op_gcd(1071, 462);
	}
	report.novlys = (uint32_t)_novlys;
	mfc_put(&report, report_ea, sizeof(report), TAG, 0, 0);
	mfc_write_tag_mask(1u << TAG);
	mfc_read_tag_status_all();
	/* an SPU thread ends with the LV2 exit, not a return from main */
	sys_spu_thread_exit(0);
	return 0;
}
