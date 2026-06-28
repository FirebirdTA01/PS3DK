#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <sys/spu.h>
#include <sys/thread.h>
#include <sys/event_queue.h>

#include <mars/task.h>

#include "spu_bin.h"

#define NUM_TASKS		10

int main(int argc,char *argv[])
{
	struct mars_task_args task_args;
	struct mars_context *mars_ctx;
	struct mars_task_id task_id[NUM_TASKS];
	int i;
	u64 barrier_ea;
	
	printf("SPU MARS sample start\n");

	sysSpuInitialize(6,0);
	sysSpuPrintfInitialize(200, NULL);

	mars_context_create(&mars_ctx, 6, 200, 100);
	mars_task_barrier_create(mars_ctx, &barrier_ea, NUM_TASKS);
	
	for(i=0;i < NUM_TASKS;i++) {
		char name[16];
		
		sprintf(name, "Task %d", i);
		
		mars_task_create(mars_ctx, &task_id[i], name, spu_bin, MARS_TASK_CONTEXT_SAVE_SIZE_MAX);
		
		task_args.type.u64[0] = barrier_ea;
		mars_task_schedule(&task_id[i], &task_args, 0);
	}

	for(i=0;i < NUM_TASKS;i++) {
		mars_task_wait(&task_id[i], NULL);
		mars_task_destroy(&task_id[i]);
	}
	
	mars_task_barrier_destroy(barrier_ea);
	mars_context_destroy(mars_ctx);
	
	printf("SPU MARS sample end\n");

	return 0;
}