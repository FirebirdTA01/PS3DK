#include <stdio.h>
#include <mars/task.h>
#include <sys/spu_printf.h>

#define ITERATIONS		3

int mars_task_main(const struct mars_task_args *task_args)
{
	int i;
	uint64_t barrier_ea = task_args->type.u64[0];
	
	spu_printf("MPU(%d): %s - Hello!\n", mars_task_get_kernel_id(), mars_task_get_name());
	
	for(i=0;i < ITERATIONS;i++) {
		spu_printf("MPU(%d): %s pre barrier work\n", mars_task_get_kernel_id(), mars_task_get_name());
		mars_task_barrier_notify(barrier_ea);
		mars_task_barrier_wait(barrier_ea);
		spu_printf("MPU(%d): %s post barrier work\n", mars_task_get_kernel_id(), mars_task_get_name());
	}
	
	return 0;
}
