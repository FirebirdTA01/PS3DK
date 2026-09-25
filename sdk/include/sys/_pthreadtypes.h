/*
 * PS3 Custom Toolchain — <sys/_pthreadtypes.h> wrapper.
 *
 * Ensures that pthread primitive types (pthread_t, pthread_mutex_t,
 * pthread_cond_t, pthread_attr_t, etc.) are always defined when
 * newlib's <sys/types.h> includes <sys/_pthreadtypes.h>, even under
 * strict ISO C modes (-std=c99, -std=c11, -ansi) where __POSIX_VISIBLE
 * is below 199506.
 */

#ifndef _PS3DK_SYS_PTHREADTYPES_H_WRAPPER
#define _PS3DK_SYS_PTHREADTYPES_H_WRAPPER

#ifdef _POSIX_THREADS
# include_next <sys/_pthreadtypes.h>
#else
# define _POSIX_THREADS 1
# include_next <sys/_pthreadtypes.h>
# undef _POSIX_THREADS
#endif

#endif /* _PS3DK_SYS_PTHREADTYPES_H_WRAPPER */
