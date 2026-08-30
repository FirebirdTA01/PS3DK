/* pthread.h wrapper -- chains to newlib via #include_next, then adds the
 * declarations newlib hides on this target plus the GNU-flavoured static
 * initialisers third-party libraries expect.
 *
 * WHY THIS WRAPPER EXISTS
 * -----------------------
 * newlib guards the ENTIRE body of its <pthread.h> behind `_POSIX_THREADS`,
 * and <sys/features.h> defines that macro only for `__rtems__`.  On this
 * target the header therefore expands to nothing at all: no prototypes, and
 * no `PTHREAD_MUTEX_INITIALIZER` / `PTHREAD_COND_INITIALIZER` /
 * `PTHREAD_ONCE_INIT` (those three are `#define`d inside the skipped body,
 * in terms of the `_PTHREAD_*_INITIALIZER` values that <sys/_pthreadtypes.h>
 * does provide).  Any autoconf probe that compiles
 *     pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
 * fails, which is why cairo and pixman used to fall back to their
 * no-threads tiers (-DCAIRO_NO_MUTEX / -DPIXMAN_NO_TLS) and lost their
 * internal locking.  The implementation behind these declarations is
 * runtime/lv2/librt/pthread.c, over the Lv-2 mutex/cond/PPU-thread
 * syscalls.
 *
 * We turn `_POSIX_THREADS` on only across the #include_next, so that the
 * prototypes come from newlib verbatim (no chance of drifting from libc's
 * own idea of them), then turn it back off again: <sys/signal.h> grows two
 * extra members inside `struct sigevent` while it is defined, and leaving
 * it on would give translation units that include <pthread.h> a different
 * sigevent layout from those that do not.
 *
 * `_UNIX98_THREAD_MUTEX_ATTRIBUTES` is deliberately NOT turned on for the
 * same reason: it appends a `type` member to `pthread_mutexattr_t`, and
 * <sys/_pthreadtypes.h> is pulled in by <sys/types.h> almost everywhere,
 * so the struct would have two different layouts in one program.  The
 * PTHREAD_MUTEX_* constants and the settype/gettype prototypes are
 * declared here instead; librt stores the type in the `recursive` member,
 * which exists in both layouts.
 */
#ifndef _PS3DK_PTHREAD_WRAPPER_H
#define _PS3DK_PTHREAD_WRAPPER_H

/* <sys/_pthreadtypes.h> publishes pthread_mutex_t and friends under
 * `defined(_POSIX_THREADS) || __POSIX_VISIBLE >= 199506`, and sets its own
 * include guard either way.  A strict-ISO dialect (-std=c99, -std=c11)
 * leaves __POSIX_VISIBLE below that bar, so a <sys/types.h> that was
 * already included in such a translation unit has consumed the header
 * without defining the types, and no later include can recover them.
 * Say so plainly rather than erroring out on a hundred unknown type
 * names.  -std=gnu99 and later are unaffected. */
#if defined(_SYS__PTHREADTYPES_H_) && !defined(_POSIX_THREADS) \
    && defined(__POSIX_VISIBLE) && (__POSIX_VISIBLE < 199506)
# error "PS3DK <pthread.h>: the pthread types were dropped by a strict-ISO <sys/types.h> included earlier in this file; build with -std=gnu99 (or later), or include <pthread.h> before <sys/types.h>."
#endif

#ifdef _POSIX_THREADS
# include_next <pthread.h>
#else
# define _POSIX_THREADS 1
# include_next <pthread.h>
# undef _POSIX_THREADS
#endif

/* Defense-in-depth: if a duplicate copy of this wrapper sits earlier on the
   include path, the shared guard above makes this #include_next resolve to the
   (skipped) duplicate instead of the real <pthread.h>, silently dropping every
   declaration.  Gated on the PS3 PPU target macro so the check runs only under
   our (GCC) toolchain, where __PTHREAD_h is newlib's guard; excluded under
   __clang__ because clang-based tooling (clangd) cannot model the -isystem
   #include_next ordering and would false-positive.  Inert under host/IDE
   analysis. */
#if defined(__lv2ppu__) && !defined(__clang__) && !defined(__PS3DK_SDK_SELFBUILD__) && !defined(__PTHREAD_h)
# error "PS3DK <pthread.h> wrapper: real <pthread.h> not reached (a duplicate wrapper copy shadowed it); keep only one copy of the PS3DK wrapper headers on the include path."
#endif

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Mutex types.
 *
 * Normally spelled by <sys/_pthreadtypes.h> under
 * _UNIX98_THREAD_MUTEX_ATTRIBUTES; see the header comment for why we do
 * not switch that on.  ERRORCHECK is accepted but behaves as NORMAL:
 * the Lv-2 mutex has recursive / not-recursive and nothing in between.
 * ------------------------------------------------------------------ */
#ifndef PTHREAD_MUTEX_NORMAL
# define PTHREAD_MUTEX_NORMAL     0
#endif
#ifndef PTHREAD_MUTEX_RECURSIVE
# define PTHREAD_MUTEX_RECURSIVE  1
#endif
#ifndef PTHREAD_MUTEX_ERRORCHECK
# define PTHREAD_MUTEX_ERRORCHECK 2
#endif
#ifndef PTHREAD_MUTEX_DEFAULT
# define PTHREAD_MUTEX_DEFAULT    3
#endif

/* ------------------------------------------------------------------ *
 * Static initialisers.
 *
 * An Lv-2 mutex id is not a compile-time constant -- it comes back from
 * syscall 100 -- so a statically initialised mutex carries a sentinel and
 * is created on first use.  0xFFFFFFFF is newlib's own
 * _PTHREAD_MUTEX_INITIALIZER; the recursive and error-checking flavours
 * take the two values below it.  Lv-2 never hands out ids in the
 * 0xFFFFFFF0..0xFFFFFFFF range, and librt also treats a zeroed
 * pthread_mutex_t as "not created yet" so that calloc'd structures behave.
 * ------------------------------------------------------------------ */
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
# define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP  ((pthread_mutex_t) 0xFFFFFFFEU)
#endif
#ifndef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
# define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP ((pthread_mutex_t) 0xFFFFFFFDU)
#endif

/* ------------------------------------------------------------------ *
 * Declarations newlib gates off on this target.
 * ------------------------------------------------------------------ */
int pthread_mutexattr_settype (pthread_mutexattr_t *__attr, int __kind);
int pthread_mutexattr_gettype (const pthread_mutexattr_t *__attr, int *__kind);

/* Declared by newlib only under _POSIX_TIMEOUTS, which is off here. */
int pthread_mutex_timedlock (pthread_mutex_t *__mutex,
                             const struct timespec *__timeout);

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_PTHREAD_WRAPPER_H */
