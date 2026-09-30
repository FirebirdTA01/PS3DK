/* hello-ovis, manual mode: the sections are listed in manual.xml, and this
 * program maps each one before calling into it.
 *
 * argument 1: overlay table EA, argument 2: report EA. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <cell/ovis.h>
#include "ops.h"

#define TAG 3

CELL_OVIS_DEFINE_MAPPER(sec_add)
CELL_OVIS_DEFINE_MAPPER(sec_mul)
CELL_OVIS_DEFINE_MAPPER(sec_fib)
CELL_OVIS_DEFINE_MAPPER(sec_gcd)

static ovis_report_t report;

/* map a section and wait for it, unless it is already resident */
#define MAP(sec, table) do { \
	if (cellOvisStartMapping_##sec(table, TAG) == CELL_OK) \
		cellOvisWaitMapping_##sec(TAG); \
} while (0)

static void round_trip(uint64_t table, uint32_t *out)
{
	MAP(sec_add, table); out[0] = op_add(40, 2);
	MAP(sec_mul, table); out[1] = op_mul(6, 7);   /* evicts add */
	MAP(sec_fib, table); out[2] = op_fib(20);
	MAP(sec_gcd, table); out[3] = op_gcd(1071, 462); /* evicts fib */
}

int main(uint64_t table, uint64_t report_ea)
{
	round_trip(table, &report.results[0]);
	round_trip(table, &report.results[4]);

	/* gcd is resident: mapping it again transfers nothing */
	report.abort_rc = (uint32_t)cellOvisStartMapping_sec_gcd(table, TAG);
	report.mapped[0] = spu_extract(__ovly_info_sec_add, 3);
	report.mapped[1] = spu_extract(__ovly_info_sec_mul, 3);
	report.mapped[2] = spu_extract(__ovly_info_sec_fib, 3);
	report.mapped[3] = spu_extract(__ovly_info_sec_gcd, 3);
	report.novlys = (uint32_t)_novlys;

	mfc_put(&report, report_ea, sizeof(report), TAG, 0, 0);
	mfc_write_tag_mask(1u << TAG);
	mfc_read_tag_status_all();
	/* an SPU thread ends with the LV2 exit, not a return from main */
	sys_spu_thread_exit(0);
	return 0;
}
