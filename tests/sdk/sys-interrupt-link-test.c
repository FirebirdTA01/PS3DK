/* tests/sdk/sys-interrupt-link-test.c: the interrupt-thread teardown library call links under both names
 * from <sys/interrupt.h> alone (liblv2_stub exports sys_interrupt_thread_disestablish and PSL1GHT's
 * sysInterruptThreadDisestablish).  Linked, not run. */
#include <sys/interrupt.h>

int main(void)
{
    return sys_interrupt_thread_disestablish(0) + sysInterruptThreadDisestablish(0);
}
