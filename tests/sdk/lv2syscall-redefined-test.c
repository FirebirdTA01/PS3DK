/*
 * tests/sdk/lv2syscall-redefined-test.c
 *
 * Verifies that including <sys/process.h> and <sys/sys_time.h> in either
 * order does not trigger macro redefinition errors (lv2syscall0..8 and
 * register_passing_1..7) under -Wall -Wextra -Werror in both C and C++.
 */

#include <stddef.h>

#ifdef ORDER_PROCESS_FIRST
#include <sys/process.h>
#include <sys/sys_time.h>
#else
#include <sys/sys_time.h>
#include <sys/process.h>
#endif

int main(void)
{
    (void)sysProcessGetPid();
    (void)sys_time_get_system_time();
    return 0;
}
