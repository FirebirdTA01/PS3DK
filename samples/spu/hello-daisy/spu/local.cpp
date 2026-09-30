/* hello-daisy SPU side: run the local-stream check on this SPU and exit with
 * the number of failures. */
#include <sys/spu_thread.h>
#include "../source/local_stream.cpp"

int main()
{
	sys_spu_thread_exit(run_local());
	return 0;
}
