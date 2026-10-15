/* The signal hook the pthread-signal-unbind build splices into librt's pthread.c (force-included: a function-like
   macro passed with -D can be lost by the CMake generator, as metalock-contested's was). */
#ifndef PTHREAD_SIGNAL_UNBIND_HOOK_H
#define PTHREAD_SIGNAL_UNBIND_HOOK_H
void test_cond_id_loaded(void);
#define PT_TEST_COND_ID_LOADED() test_cond_id_loaded()
#endif
