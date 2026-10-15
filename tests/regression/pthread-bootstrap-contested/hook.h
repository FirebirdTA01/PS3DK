/* The bootstrap hooks the pthread-bootstrap-contested build splices into librt's pthread.c (force-included: a
   function-like macro passed with -D can be lost by the CMake generator). */
#ifndef PTHREAD_BOOTSTRAP_CONTESTED_HOOK_H
#define PTHREAD_BOOTSTRAP_CONTESTED_HOOK_H
void test_boot_claimed(void);
void test_boot_waiting(void);
#define PT_TEST_BOOT_CLAIMED() test_boot_claimed()
#define PT_TEST_BOOT_WAITING() test_boot_waiting()
#endif
