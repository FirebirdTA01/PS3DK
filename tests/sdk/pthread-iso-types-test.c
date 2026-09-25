/*
 * pthread-iso-types-test.c — Verify pthread types available under strict ISO C/C++
 *
 * Tests that <sys/types.h> included before <pthread.h> does NOT drop pthread
 * primitive types (pthread_t, pthread_mutex_t, pthread_cond_t, etc.) even
 * under strict ISO modes (-std=c99, -std=c11, -ansi) where __POSIX_VISIBLE < 199506.
 */

#if defined(TEST_ORDER_SYS_TYPES_THEN_PTHREAD)
# include <sys/types.h>
# include <pthread.h>
#elif defined(TEST_ORDER_PTHREAD_THEN_SYS_TYPES)
# include <pthread.h>
# include <sys/types.h>
#else
# include <sys/types.h>
# include <pthread.h>
#endif

#include <stddef.h>

int check_pthread_types(void)
{
    pthread_t tid = (pthread_t)0;
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_attr_t attr;
    pthread_key_t key;

    (void)tid;
    (void)mtx;
    (void)cond;
    (void)once;
    (void)attr;
    (void)key;

    return 0;
}
