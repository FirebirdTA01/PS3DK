/*
 *   SCE CONFIDENTIAL                                       
 *   PlayStation(R)3 Programmer Tool Runtime Library 475.001 
 *   Copyright (C) 2008 Sony Computer Entertainment Inc.    
 *   All Rights Reserved.                                   
 */

/*E
 * File: event_flag.ppu.c
 * Description:
 * This sample shows an usage example of Event Flag. 
 *
 * This program uses Event Flag to synchronize between Master and Workers.
 * There are two event_flags in the program. One is for the master the other 
 * is for workers. 
 * The master waits for events from workers. One event is corresponded to each 
 * Workers. When workers end their own job, they set a event to the 
 * corresponding bit of the master`s flags.
 * Then the master awakes and checks results of their jobs, and notify to the 
 * event_flag of workers.
 * 
 * The communication scenario between the master and workers is followings.
 *
 * a) PPU - PPU
 *  1. The master waits for events from workers by sys_event_flag_wait(). 
 *  2. When workers finish their own job, they notify to the master by 
 *     sys_event_flag_set(). 
 *  3. The master checks results of worker`s jobs.
 *  4. Workers wait for the end of checking by sys_event_flag_wait().
 *  5. The master notifies to workers by sys_event_flag_set().
 *
 * b) PPU - SPU
 *  1. The master waits for a event from the SPU worker by 
 *     sys_event_flag_wait(). 
 *  2. When the worker finishes an own job, it notifies to the master by 
 *     sys_event_flag_set_bit(). 
 */

#include <stdio.h>
#include <stdlib.h>
#include <ppu_intrinsics.h>
#include <spu_printf.h>

#include <sys/ppu_thread.h>
#include <sys/synchronization.h>
#include <sys/process.h>
#include <sys/time_util.h>
#include <sys/spu_image.h>
#include <sys/spu_thread_group.h>
#include <sys/spu_thread.h>
#include "spu_ef.h"

#include <sys/paths.h>

/* SPU ELF embedded into the PPU ELF via PSL1GHT's bin2o macro
 * (see Makefile + data/spu_ef_bin).  The reference Sony sample loads
 * an SPU SELF off /app_home/ via sys_spu_image_open; we keep the
 * sample self-contained by importing an embedded blob with
 * sys_spu_image_import instead. */
#include "spu_ef_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000)

#define PPU_THREAD_STACK_SIZE    4096 /*E Stack size                    */
#define EVENT_FLAG_INIT             0 /*E Event Flag initial value      */
#define NUM_OF_WORKERS              5 /*E The number of worker threads  */
#define PPU_THREAD_PRIO          1500 /*E Thread priority               */

int table[NUM_OF_WORKERS];            /*E Result table of worker`s jobs */

struct spu_handle {
	sys_spu_image_t img;
	sys_spu_thread_group_t group;
	int num;
	sys_spu_thread_t *threads;
};

typedef struct spu_handle spu_handle_t;

sys_event_flag_t eflag_m;             /*E Event Flag for Master         */
sys_event_flag_t eflag_w;             /*E Event Flag for Workers        */

void master_func(uint64_t arg);
void worker_func(uint64_t arg);

void spu_start(spu_handle_t **handle, const char *prog, int num);
void spu_end(spu_handle_t *handle);
void spu_thread_set_bit(spu_handle_t *handle, int snum, 
						sys_event_flag_t ef, int bitn);

/*E
 * Master thread entry
 */
void master_func(uint64_t arg)
{

	(void)arg; /*E This thread does not use the argument */

	sys_ppu_thread_t me;
	int my_prio;
	int ret;
	uint64_t flags = 0;
	uint64_t result;
	int idx;
	uint64_t seed;
	int my_val;
	int worker_jobs = NUM_OF_WORKERS;

	printf("Master is starting.\n");

	sys_ppu_thread_get_id(&me);
	sys_ppu_thread_get_priority(me, &my_prio);

	/*E The master creates worker PPU threads. */
	for (uint64_t n = 0; n < NUM_OF_WORKERS; n++) {
		sys_ppu_thread_t tmp;
		ret = sys_ppu_thread_create(&tmp,  worker_func, 
									n,  (my_prio + (int)n),
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
	srand((int)(seed));

	/*E This value is used when the master checks worker`s job */
	my_val = rand();

	/*E This loop continues until all workers complete their own jobs. */
	while(worker_jobs != 0) {

		/*E
		 * Step 1.
		 * The master waits for each job of workers by sys_event_flag_wait().
		 * Each bit of flags is set when a worker ends own job.
		 */
		ret = 
			sys_event_flag_wait(eflag_m,                  /*E ID for Master */
								flags,                     /*E Bit pattern  */
								SYS_EVENT_FLAG_WAIT_OR |   /*E OR mode      */
								SYS_EVENT_FLAG_WAIT_CLEAR, /*E Clear option */
								&result,                   /*E Result value */
								SYS_NO_TIMEOUT);           /*E Timeout      */
		
		if (ret != CELL_OK) {
			printf("Master:: sys_event_flag_wait failed %x\n",ret);
			exit(ret);
		}

		/*E
		 * Step 3.
		 * The master checks results of worker`s jobs.
		 */
		do {
			--worker_jobs;

			idx = 63 - __cntlzd(result);

			/*E A result of a worker`s job is checked here */
			table[idx] = ((my_val <= table[idx]) ? 0 : my_val);

			result &= ~(1ULL << idx);

			/*E 
			 * Step 5. 
			 * The master notifies to a worker that the master has finished 
			 * the check by sys_event_flag_set().
			 */
			ret = sys_event_flag_set(eflag_w,          /*E ID for workers   */
									 (1ULL << idx));      /*E Worker`s bit  */
			if (ret != CELL_OK) {
				printf("Master:: sys_event_flag_set failed %x\n",ret);
				exit(ret);
			}

		} while(result != 0);
	}



	/*E
	 * Followings are simple usages of between PPU Thread and SPU Thread.
	 * Master PPU thread waits for the end of worker SPU Thread job.
	 */

	spu_handle_t *handle;

	/*E 
	 * This function creates a SPU thread group and SPU threads.
	 * And it starts them.
	 */
	spu_start(&handle, NULL, 1);  /* path arg unused with embedded blob */

	int set_bit = (NUM_OF_WORKERS + 1);
	spu_thread_set_bit(handle, 0, eflag_m, set_bit);

	/*E
	 *  1. The master waits for a event from the SPU worker by 
	 *     sys_event_flag_wait().
	 */ 
	ret = 
		sys_event_flag_wait(eflag_m,                  /*E ID for Master */
							(1ULL << set_bit),         /*E Bit pattern  */
							SYS_EVENT_FLAG_WAIT_OR |   /*E OR mode      */
							SYS_EVENT_FLAG_WAIT_CLEAR, /*E Clear option */
							&result,                   /*E Result value */
							SYS_NO_TIMEOUT);           /*E Timeout      */
		
	if (ret != CELL_OK) {
		printf("Master:: sys_event_flag_wait failed %x\n",ret);
		exit(ret);
	}

	/*E
	 * This function destroys a SPU thread group and SPU thread.
	 */
	spu_end(handle);

	printf("Master is exiting.\n");

	sys_ppu_thread_exit(0);
}


/*E
 * Worker thread entry
 */
void worker_func(uint64_t thr_num)
{
	int ret;
	uint64_t seed;

	printf("Worker[%llu] is starting.\n",thr_num);

	/*E Worker`s job */
	SYS_TIMEBASE_GET(seed);
	srand((int)(seed));
	table[thr_num] = rand();


	/*E
	 * Step 2. 
	 * Workers have finished their own job. They sets a corresponding event
	 * by sys_event_flag_set().
	 */
	ret = sys_event_flag_set(eflag_m,                   /*E ID for Master */
							 (1ULL << thr_num));        /*E bit pattern   */
	if (ret != CELL_OK) {
		printf("WORKER:: sys_event_flag_set failed %x\n",ret);
		exit(ret);
	}

	/*E 
	 * Step 4.
	 * Workers wait for the end of the check by sys_event_flag_wait().
	 * The master sets a event to the corresponding bit.
	 */
	ret = sys_event_flag_wait(eflag_w,                  /*E ID for workers */
							  (1ULL << thr_num),         /*E Bit pattern   */
							  SYS_EVENT_FLAG_WAIT_AND,   /*E AND mode      */
							  0,                         /*E Result value  */
							  SYS_NO_TIMEOUT);           /*E Timeout       */
		
	if (ret != CELL_OK) {
		printf("WORKER:: sys_event_flag_wait failed %x\n",ret);
		exit(ret);
	}

	if (table[thr_num] == 0) {
		printf("Worker[%llu] succeeded my job\n",thr_num);
	}

	printf("Worker[%llu] is exiting.\n", thr_num);
	sys_ppu_thread_exit(0);
}


/*E
 *  main
 */
int main(int argc, char **argv)
{
	(void)argc;

	int ret;
	sys_ppu_thread_t master;

	spu_printf_initialize(100, NULL);

	printf("%s is starting.\n", argv[0]);

	sys_event_flag_attribute_t attr;
	sys_event_flag_attribute_initialize(attr);

	/*E The master`s event flag creation */
	ret = sys_event_flag_create(&eflag_m, &attr, EVENT_FLAG_INIT);
	if (ret != CELL_OK) {
		printf("sys_event_flag_create failed %x\n",ret);
		exit(ret);
	}

	/*E Worker`s event flag creation */
	ret = sys_event_flag_create(&eflag_w, &attr, EVENT_FLAG_INIT);
	if (ret != CELL_OK) {
		printf("sys_event_flag_create failed %x\n",ret);
		exit(ret);
	}

	/*E Master thread creation */
	ret = sys_ppu_thread_create(&master, master_func, 
								NUM_OF_WORKERS, PPU_THREAD_PRIO,
								PPU_THREAD_STACK_SIZE,
								SYS_PPU_THREAD_CREATE_JOINABLE,
								"Master");
	if (ret != CELL_OK) {
		printf("sys_ppu_thread_create failed %x\n", ret);
		exit(ret);
	}


	uint64_t val;
	sys_ppu_thread_join(master, &val);

	sys_event_flag_destroy(eflag_m);
	sys_event_flag_destroy(eflag_w);

	printf("%s is exiting.\n", argv[0]);
	
	return 0;
}


/*E
 * This function creates a SPU Thread Group and SPU Threads,
 * and starts them. 
 */
void
spu_start(spu_handle_t **handle, const char *prog, int num)
{
	spu_handle_t *hd;
	int ret;

	hd = (spu_handle_t*)(malloc(sizeof(spu_handle_t)));
	if (hd == NULL) {
		printf("malloc failed \n");
		exit(-1);
	}

	hd->threads = 
		(sys_spu_thread_t*)(malloc(sizeof(sys_spu_thread_t) * num));
	if (hd->threads == NULL) {
		printf("malloc failed \n");
		exit(-1);
	}

	(void)prog;  /* unused: we use an embedded blob instead of /app_home/ path */
	ret = sys_spu_image_import(&(hd->img), spu_ef_bin, 0);
	if (ret != CELL_OK) {
		printf("sys_spu_image_import failed: 0x%x\n", ret);
		exit(ret);
	}

	sys_spu_thread_group_attribute_t gattr;
	sys_spu_thread_argument_t arg;
	sys_spu_thread_attribute_t tattr;

	sys_spu_thread_attribute_initialize(tattr);
	sys_spu_thread_argument_initialize(arg);
	sys_spu_thread_group_attribute_initialize(gattr);
	sys_spu_thread_group_attribute_name(gattr, "group");

	ret = sys_spu_thread_group_create(&(hd->group), num, 100, &gattr);
	if (ret != CELL_OK) {
		printf("sys_spu_thread_group_create failed\n");
		exit(-1);
	}

	for (int i = 0; i < num; i++) {
		ret = sys_spu_thread_initialize(&(hd->threads[i]), hd->group, 
										i, &(hd->img),
										&tattr, &arg);
		if (ret != CELL_OK) {
			printf("sys_spu_thread_initialize failed\n");
			exit(ret);
		}
	}

	spu_printf_attach_group(hd->group);

	ret = sys_spu_thread_group_start(hd->group);
	if (ret != CELL_OK) {
		printf("sys_spu_thread_group_start failed \n");
		exit(ret);
	}

	hd->num = num;
	*handle = hd;

	return;
}


/*E
 * This function ends SPU Threads.
 */
void
spu_end(spu_handle_t *handle)
{
	int ret;

	for (int i = 0; i < handle->num; i++) {
		ret = sys_spu_thread_write_snr(handle->threads[i], 0,
									   MAKE_COMMAND(SPU_COMMAND_END, 0));
		if (ret != CELL_OK) {
			printf("sys_spu_thread_write_snr failed\n");
			exit(ret);
		}
	}

	int cause, value;
	ret = sys_spu_thread_group_join(handle->group, &cause, &value);
	if (ret != CELL_OK) {
		printf("sys_spu_thread_group_join failed\n");
		exit(ret);
	}

	if (cause != SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT) {
		printf("cause is wrong %x\n",cause);
		exit(-1);
	}

	for (int i = 0; i < handle->num; i++) {
		int stat;
		ret = sys_spu_thread_get_exit_status(handle->threads[i], &stat);
		if (ret != CELL_OK) {
			printf("sys_spu_thread_get_exit_status failed\n");
			exit(ret);
		}
		if (stat != CELL_OK) {
			printf("stat is wrong %x\n",stat);
			exit(-1);
		}
	}

	free(handle->threads);
	free(handle);

	return;
}

/*E
 * This function notifies the event_flag ID and the set-bit command 
 * to the SPU Thread.
 */
void
spu_thread_set_bit(spu_handle_t *handle, int snum, sys_event_flag_t ef,
				   int bitn)
{
	int ret;

	ret = sys_spu_thread_write_snr(handle->threads[snum], 0,
								   MAKE_COMMAND(SPU_COMMAND_SET_BIT, bitn));
	if (ret != CELL_OK) {
		printf("sys_spu_thread_write_snr failed\n");
		exit(ret);
	}

	ret = sys_spu_thread_write_snr(handle->threads[snum], 1, ef);
	if (ret != CELL_OK) {
		printf("sys_spu_thread_write_snr failed\n");
		exit(ret);
	}
	return;
}

