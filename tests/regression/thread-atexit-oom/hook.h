/* thread-atexit-oom: librt's pthread.c built with the thread_local destructor registration's allocation failing
   (force-included). */
#ifndef THREAD_ATEXIT_OOM_HOOK_H
#define THREAD_ATEXIT_OOM_HOOK_H
#define PT_TEST_TLS_ALLOC(size) ((void *)0)
#endif
