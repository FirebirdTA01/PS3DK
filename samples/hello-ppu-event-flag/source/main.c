/*
 * hello-ppu-event-flag — PPU port of a reference lv2/event_flag sample.
 *
 * PPU master/worker synchronisation via sys_event_flag_*.  The SPU
 * half of the original (which sets event-flag bits via
 * sys_spu_thread_write_snr) is dropped; this is the "PPU - PPU"
 * scenario.
 *
 * Needed five new compat headers under <sys/synchronization.h> +
 * <sys/ppu_thread.h> + <sys/time_util.h> + <sys/return_code.h> +
 * <cell/error.h> on top of the existing <sys/process.h> already
 * shipped by PSL1GHT.  Source flow is otherwise identical to the
 * reference sample minus the SPU bits.
 *
 * Imports lv2 syscalls only — no SPRX modules, no nidgen-built stub
 * archives.  Each sys_event_flag_* call goes straight into the lv2
 * kernel via the lv2syscallN macro.
 */

#include <stdio.h>
#include <stdlib.h>

#include <sys/ppu_thread.h>
#include <sys/synchronization.h>
#include <sys/process.h>
#include <sys/time_util.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define PPU_THREAD_STACK_SIZE    4096
#define EVENT_FLAG_INIT             0
#define NUM_OF_WORKERS              5
#define PPU_THREAD_PRIO          1500

static int                table[NUM_OF_WORKERS];
static sys_event_flag_t   eflag_m;
static sys_event_flag_t   eflag_w;

static void worker_func(uint64_t thr_num)
{
	int       ret;
	uint64_t  seed;

	printf("Worker[%llu] is starting.\n", (unsigned long long)thr_num);

	SYS_TIMEBASE_GET(seed);
	srand((int)seed);
	table[thr_num] = rand();

	/* Step 2: notify master that this worker is done. */
	ret = sys_event_flag_set(eflag_m, (1ULL << thr_num));
	if (ret != CELL_OK) {
		printf("WORKER:: sys_event_flag_set failed %x\n", ret);
		exit(ret);
	}

	/* Step 4: wait for master ack. */
	ret = sys_event_flag_wait(eflag_w, (1ULL << thr_num),
	                          SYS_EVENT_FLAG_WAIT_AND, 0, SYS_NO_TIMEOUT);
	if (ret != CELL_OK) {
		printf("WORKER:: sys_event_flag_wait failed %x\n", ret);
		exit(ret);
	}

	if (table[thr_num] == 0) {
		printf("Worker[%llu] succeeded my job\n", (unsigned long long)thr_num);
	}

	printf("Worker[%llu] is exiting.\n", (unsigned long long)thr_num);
	sys_ppu_thread_exit(0);
}

static void master_func(uint64_t arg)
{
	(void)arg;

	sys_ppu_thread_t me;
	int      my_prio;
	int      ret;
	uint64_t flags = 0;
	uint64_t result;
	int      idx;
	uint64_t seed;
	int      my_val;
	int      worker_jobs = NUM_OF_WORKERS;

	printf("Master is starting.\n");

	sys_ppu_thread_get_id(&me);
	sys_ppu_thread_get_priority(me, &my_prio);

	for (uint64_t n = 0; n < NUM_OF_WORKERS; n++) {
		sys_ppu_thread_t tmp;
		ret = sys_ppu_thread_create(&tmp, worker_func,
		                            n, (my_prio + (int)n),
		                            PPU_THREAD_STACK_SIZE,
		                            SYS_PPU_THREAD_CREATE_JOINABLE,
		                            "Worker");
		if (ret != CELL_OK) {
			printf("sys_ppu_thread_create failed %x\n", ret);
			exit(ret);
		}
		flags |= (1ULL << n);
	}

	SYS_TIMEBASE_GET(seed);
	srand((int)seed);
	my_val = rand();

	while (worker_jobs != 0) {
		/* Step 1: wait for any worker. */
		ret = sys_event_flag_wait(eflag_m, flags,
		                          SYS_EVENT_FLAG_WAIT_OR | SYS_EVENT_FLAG_WAIT_CLEAR,
		                          &result, SYS_NO_TIMEOUT);
		if (ret != CELL_OK) {
			printf("Master:: sys_event_flag_wait failed %x\n", ret);
			exit(ret);
		}

		do {
			--worker_jobs;
			idx = 63 - __builtin_clzll(result);
			table[idx] = ((my_val <= table[idx]) ? 0 : my_val);
			result &= ~(1ULL << idx);

			/* Step 5: ack the worker. */
			ret = sys_event_flag_set(eflag_w, (1ULL << idx));
			if (ret != CELL_OK) {
				printf("Master:: sys_event_flag_set failed %x\n", ret);
				exit(ret);
			}
		} while (result != 0);
	}

	printf("Master is exiting.\n");
	sys_ppu_thread_exit(0);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int               ret;
	sys_ppu_thread_t  master;
	uint64_t          val;
	sys_event_flag_attribute_t attr;

	printf("hello-ppu-event-flag: lv2/event_flag PPU port\n");

	sys_event_flag_attribute_initialize(attr);

	ret = sys_event_flag_create(&eflag_m, &attr, EVENT_FLAG_INIT);
	if (ret != CELL_OK) {
		printf("sys_event_flag_create (master) failed %x\n", ret);
		exit(ret);
	}

	ret = sys_event_flag_create(&eflag_w, &attr, EVENT_FLAG_INIT);
	if (ret != CELL_OK) {
		printf("sys_event_flag_create (worker) failed %x\n", ret);
		exit(ret);
	}

	ret = sys_ppu_thread_create(&master, master_func,
	                            NUM_OF_WORKERS, PPU_THREAD_PRIO,
	                            PPU_THREAD_STACK_SIZE,
	                            SYS_PPU_THREAD_CREATE_JOINABLE,
	                            "Master");
	if (ret != CELL_OK) {
		printf("sys_ppu_thread_create (master) failed %x\n", ret);
		exit(ret);
	}

	sys_ppu_thread_join(master, &val);

	sys_event_flag_destroy(eflag_m);
	sys_event_flag_destroy(eflag_w);

	printf("hello-ppu-event-flag: done\n");
	return 0;
}
