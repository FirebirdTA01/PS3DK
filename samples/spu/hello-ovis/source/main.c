/* hello-ovis: run two overlaid SPU programs and check their reports.
 *
 * For each program: size and fill its overlay table from the embedded ELF
 * (cellOvisGetOverlayTableSize / cellOvisInitializeOverlayTable), build a
 * user image from its segments (sys_spu_elf_get_segments), drop the overlay
 * segments from it (cellOvisFixSpuSegments), and run it as an SPU thread with
 * the table and a report buffer as arguments.
 *
 * Prints HELLO_OVIS <program> PASS lines and HELLO_OVIS DONE passed=N of M. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <sys/process.h>
#include <sys/spu_image.h>
#include <sys/spu_initialize.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/spu_utility.h>
#include <cell/ovis.h>

#include "ops.h"
#include "ovis_manual_bin.h"
#include "ovis_auto_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const uint32_t expect[4] = { 42, 42, 6765, 21 };

static int checks, passed;

static void check(const char *prog, const char *what, uint32_t got, uint32_t want)
{
	checks++;
	if (got == want) {
		passed++;
	} else {
		printf("HELLO_OVIS %s FAIL %s got=0x%x want=0x%x\n", prog, what, (unsigned)got, (unsigned)want);
	}
}

static int run(const char *prog, const void *elf, int manual)
{
	sys_spu_image_t img;
	sys_spu_thread_group_t group;
	sys_spu_thread_t thread;
	int cause, status, rc, i, nsegs_before;
	int start = passed, start_checks = checks;
	ovis_report_t *report = memalign(128, sizeof(*report));

	int size = cellOvisGetOverlayTableSize(elf);
	void *table = memalign(128, size > 0 ? size : 128);
	check(prog, "table size", size > 0, 1);
	rc = cellOvisInitializeOverlayTable(table, elf);
	check(prog, "table init", (uint32_t)rc, CELL_OK);
	check(prog, "misaligned table refused",
	      (uint32_t)cellOvisInitializeOverlayTable((char *)table + 16, elf), (uint32_t)CELL_OVIS_ERROR_ALIGN);
	check(prog, "non-ELF refused", (uint32_t)cellOvisGetOverlayTableSize("not an elf"), (uint32_t)CELL_OVIS_ERROR_INVAL);
	memset(report, 0xee, sizeof(*report));

	/* a user image: the segment list lives here, so it can be edited */
	uint32_t entry;
	int nsegs;
	rc = sys_spu_elf_get_information((sys_addr_t)(uintptr_t)elf, &entry, &nsegs);
	if (rc != CELL_OK) {
		printf("HELLO_OVIS %s FAIL elf information 0x%x\n", prog, rc);
		return 0;
	}
	sys_spu_segment_t *segs = memalign(128, sizeof(*segs) * nsegs);
	rc = sys_spu_elf_get_segments((sys_addr_t)(uintptr_t)elf, segs, nsegs);
	if (rc != CELL_OK) {
		printf("HELLO_OVIS %s FAIL elf segments 0x%x\n", prog, rc);
		return 0;
	}
	img.type = SYS_SPU_IMAGE_TYPE_USER;
	img.entry_point = entry;
	img.segs = segs;
	img.nsegs = nsegs;
	nsegs_before = img.nsegs;
	for (i = 0; i < img.nsegs; i++)
		printf("  %s seg %d: type %d ls 0x%05x size 0x%x\n", prog, i, img.segs[i].type,
		       (unsigned)img.segs[i].ls_start, (unsigned)img.segs[i].size);
	cellOvisFixSpuSegments(&img);
	printf("  %s segments %d -> %d\n", prog, nsegs_before, img.nsegs);
	/* the overlay segments went (ld may merge neighbouring sections into
	 * one segment, so count what is left rather than what went): no two
	 * surviving load segments share LS */
	check(prog, "overlay segments dropped", img.nsegs < nsegs_before, 1);
	{
		int j, clash = 0;
		for (i = 0; i < img.nsegs; i++)
			for (j = i + 1; j < img.nsegs; j++)
				if (img.segs[i].type != SYS_SPU_SEGMENT_TYPE_INFO &&
				    img.segs[j].type != SYS_SPU_SEGMENT_TYPE_INFO &&
				    img.segs[i].ls_start < img.segs[j].ls_start + (uint32_t)img.segs[j].size &&
				    img.segs[j].ls_start < img.segs[i].ls_start + (uint32_t)img.segs[i].size)
					clash = 1;
		check(prog, "no overlapping segments left", clash, 0);
	}

	sys_spu_thread_group_attribute_t ga = { .nsize = 5, .name = "ovis", .type = 0 };
	sys_spu_thread_attribute_t ta = { .name = "ovis", .nsize = 5, .option = SPU_THREAD_ATTR_NONE };
	sys_spu_thread_argument_t arg = { (uint64_t)(uintptr_t)table, (uint64_t)(uintptr_t)report, 0, 0 };
	rc = sys_spu_thread_group_create(&group, 1, 100, &ga);
	if (rc == CELL_OK)
		rc = sys_spu_thread_initialize(&thread, group, 0, &img, &ta, &arg);
	if (rc == CELL_OK)
		rc = sys_spu_thread_group_start(group);
	if (rc == CELL_OK)
		rc = sys_spu_thread_group_join(group, &cause, &status);
	sys_spu_thread_group_destroy(group);
	check(prog, "spu thread", (uint32_t)rc, CELL_OK);
	check(prog, "spu exit status", (uint32_t)status, 0);

	for (i = 0; i < 8; i++) {
		char what[32];
		snprintf(what, sizeof(what), "result %d", i);
		check(prog, what, report->results[i], expect[i % 4]);
	}
	check(prog, "section count", report->novlys, 4);
	if (manual) {
		/* gcd was resident: a second StartMapping transfers nothing */
		check(prog, "resident section refused", report->abort_rc, (uint32_t)0x8041040C);
		/* the last round left mul and gcd resident, add and fib evicted */
		check(prog, "add evicted", report->mapped[0], 0);
		check(prog, "mul resident", report->mapped[1], 1);
		check(prog, "fib evicted", report->mapped[2], 0);
		check(prog, "gcd resident", report->mapped[3], 1);
	}
	free(segs);
	free(table);
	free(report);
	printf("HELLO_OVIS %s %s (%d of %d)\n", prog, passed - start == checks - start_checks ? "PASS" : "FAIL",
	       passed - start, checks - start_checks);
	return passed - start == checks - start_checks;
}

int main(void)
{
	sys_spu_initialize(6, 0);
	run("manual", ovis_manual_bin, 1);
	run("auto", ovis_auto_bin, 0);
	printf("HELLO_OVIS DONE passed=%d of %d\n", passed, checks);
	fflush(stdout);
	return passed == checks ? 0 : 1;
}
