/* hello-daisy: libdaisy data streams.
 *
 *   1. a local stream on the PPU (one thread)
 *   2. a local stream between two PPU threads, 10000 entries in order
 *   3. the same local-stream check inside an SPU thread
 *   4. an atomic stream from the PPU to two SPU consumers (5000 entries)
 *   5. an atomic stream from one SPU to another
 *
 * Prints HELLO_DAISY <check> PASS/FAIL and HELLO_DAISY DONE passed=N of M. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/ppu_thread.h>
#include <sys/spu_image.h>
#include <sys/spu_initialize.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <cell/daisy.h>

#include "daisy_common.h"
#include "spu_local_bin.h"
#include "spu_atomic_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

int run_local();

using namespace cell::Daisy;

static int checks, passed;

static void report(const char *what, int fails)
{
	checks++;
	if (fails == 0)
		passed++;
	printf("HELLO_DAISY %s %s (%d failures)\n", what, fails ? "FAIL" : "PASS", fails);
}

/* ---- two PPU threads ------------------------------------------------ */

typedef Buffer::Local<Word, 8> WordBuffer;
static Lock gLock __attribute__((aligned(128)));
static WordBuffer gBuffer;
static const uint32_t THREAD_COUNT = 10000;
static int gConsumerFails;

static void consumer(uint64_t)
{
	QueueControl::Local<8, OUTPUT> q(gLock);
	Pipe::OutPort<WordBuffer, QueueControl::Local<8, OUTPUT> > out(gBuffer, q);
	uint32_t expect = 0;
	Word w;
	int ret;
	while ((ret = out.pop(&w)) == CELL_OK) {
		if (w.v != expect)
			gConsumerFails++;
		expect++;
	}
	if (ret != TERMINATED || expect != THREAD_COUNT)
		gConsumerFails++;
	sys_ppu_thread_exit(0);
}

static int run_threads()
{
	memset((void *)&gLock, 0, sizeof(gLock));
	QueueControl::Local<8, INPUT> q(gLock);
	sys_ppu_thread_t tid;
	if (sys_ppu_thread_create(&tid, consumer, 0, 1000, 0x4000, SYS_PPU_THREAD_CREATE_JOINABLE, "daisy") != CELL_OK)
		return 1;
	{
		Pipe::InPort<WordBuffer, QueueControl::Local<8, INPUT> > in(gBuffer, q);
		for (uint32_t i = 0; i < THREAD_COUNT; i++) {
			Word w = { i, {0, 0, 0} };
			in.push(&w);
		}
		/* the destructor terminates and waits for the consumer to detach */
	}
	uint64_t status;
	sys_ppu_thread_join(tid, &status);
	return gConsumerFails;
}

/* ---- an SPU thread ---------------------------------------------------- */

static int run_spu()
{
	sys_spu_image_t img;
	sys_spu_thread_group_t group;
	sys_spu_thread_t thread;
	int cause, status = -1;
	if (sys_spu_image_import(&img, spu_local_bin, SYS_SPU_IMAGE_PROTECT) != CELL_OK)
		return 1;
	sys_spu_thread_group_attribute_t ga = { .nsize = 6, .name = "daisy", .type = 0 };
	sys_spu_thread_attribute_t ta = { .name = "daisy", .nsize = 6, .option = SPU_THREAD_ATTR_NONE };
	sys_spu_thread_argument_t arg = { 0, 0, 0, 0 };
	int rc = sys_spu_thread_group_create(&group, 1, 100, &ga);
	if (rc == CELL_OK)
		rc = sys_spu_thread_initialize(&thread, group, 0, &img, &ta, &arg);
	if (rc == CELL_OK)
		rc = sys_spu_thread_group_start(group);
	if (rc == CELL_OK)
		rc = sys_spu_thread_group_join(group, &cause, &status);
	sys_spu_thread_group_destroy(group);
	sys_spu_image_close(&img);
	return rc != CELL_OK ? 1 : status;
}


/* ---- atomic streams --------------------------------------------------- */

static LFQueue2 gQueue;
static Word gEntries[DEPTH] __attribute__((aligned(128)));
static Result gResults[2];

/* start one SPU thread group running spu_atomic with a role per thread */
static int run_group(sys_spu_thread_group_t *group, sys_spu_thread_t *threads, int n,
                     const uint64_t *roles, sys_spu_image_t *img)
{
	sys_spu_thread_group_attribute_t ga = { .nsize = 6, .name = "daisy", .type = 0 };
	sys_spu_thread_attribute_t ta = { .name = "daisy", .nsize = 6, .option = SPU_THREAD_ATTR_NONE };
	int rc = sys_spu_thread_group_create(group, n, 100, &ga);
	for (int i = 0; rc == CELL_OK && i < n; i++) {
		sys_spu_thread_argument_t arg = { (uint64_t)(uintptr_t)&gQueue, (uint64_t)(uintptr_t)gEntries,
		                                  roles[i], (uint64_t)(uintptr_t)&gResults[i] };
		rc = sys_spu_thread_initialize(&threads[i], *group, i, img, &ta, &arg);
	}
	return rc;
}

static int check_results(int consumers)
{
	uint64_t count = 0, sum = 0;
	int fails = 0;
	for (int i = 0; i < consumers; i++) {
		count += gResults[i].count;
		sum += gResults[i].sum;
		fails += gResults[i].faults;
	}
	if (count != COUNT || sum != (uint64_t)COUNT * (COUNT - 1) / 2)
		fails++;
	return fails;
}

static int run_ppu_to_spus()
{
	sys_spu_image_t img;
	sys_spu_thread_group_t group;
	sys_spu_thread_t threads[2];
	int cause, status, fails = 0;
	memset(&gQueue, 0, sizeof(gQueue));
	memset(gResults, 0, sizeof(gResults));
	if (sys_spu_image_import(&img, spu_atomic_bin, SYS_SPU_IMAGE_PROTECT) != CELL_OK)
		return 1;
	const uint64_t roles[2] = { 0, 0 };
	int rc = run_group(&group, threads, 2, roles, &img);
	{
		/* the PPU end is constructed before the SPUs start */
		Buffer::Local<Word, DEPTH> buf(gEntries);
		QueueControl::Atomic<DEPTH, INPUT> q(&gQueue, threads, 2);
		Pipe::InPort<Buffer::Local<Word, DEPTH>, QueueControl::Atomic<DEPTH, INPUT> > in(buf, q);
		if (rc == CELL_OK)
			rc = sys_spu_thread_group_start(group);
		for (uint32_t i = 0; rc == CELL_OK && i < COUNT; i++) {
			Word w = { i, {0, 0, 0} };
			in.push(&w);
		}
	}
	if (rc == CELL_OK)
		rc = sys_spu_thread_group_join(group, &cause, &status);
	sys_spu_thread_group_destroy(group);
	sys_spu_image_close(&img);
	if (rc != CELL_OK)
		return 1;
	return fails + check_results(2);
}

static int run_spu_to_spu()
{
	sys_spu_image_t img;
	sys_spu_thread_group_t group;
	sys_spu_thread_t threads[2];
	int cause, status;
	memset(&gQueue, 0, sizeof(gQueue));
	memset(gResults, 0, sizeof(gResults));
	if (sys_spu_image_import(&img, spu_atomic_bin, SYS_SPU_IMAGE_PROTECT) != CELL_OK)
		return 1;
	const uint64_t roles[2] = { 0, 1 };  /* thread 0 consumes, thread 1 produces */
	int rc = run_group(&group, threads, 2, roles, &img);
	if (rc == CELL_OK)
		rc = sys_spu_thread_group_start(group);
	if (rc == CELL_OK)
		rc = sys_spu_thread_group_join(group, &cause, &status);
	sys_spu_thread_group_destroy(group);
	sys_spu_image_close(&img);
	if (rc != CELL_OK)
		return 1;
	return check_results(1);
}

int main()
{
	sys_spu_initialize(6, 0);
	report("ppu local", run_local());
	report("ppu threads", run_threads());
	report("spu local", run_spu());
	report("ppu to 2 spus (atomic)", run_ppu_to_spus());
	report("spu to spu (atomic)", run_spu_to_spu());
	printf("HELLO_DAISY DONE passed=%d of %d\n", passed, checks);
	return passed == checks ? 0 : 1;
}
