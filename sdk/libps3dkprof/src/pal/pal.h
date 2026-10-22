/* The ps3dk-prof platform layer: everything the portable core needs from the OS.  Every call that can fail returns
 * 0 on success or a negative value, and the core checks it.  Two implementations: pal_host.c (POSIX, for the host B
 * tests) and pal_lv2.c (LV2, step 3 target half, not yet written: codex approved the host half only). */
#ifndef PS3DK_PROF_PAL_H
#define PS3DK_PROF_PAL_H

#include <stddef.h>
#include <stdint.h>

/* The PAL keeps short names in the source; the archive exports them only under the reserved ps3dk_prof__ prefix. */
#define pal_alloc ps3dk_prof__pal_alloc
#define pal_free ps3dk_prof__pal_free
#define pal_mutex_init ps3dk_prof__pal_mutex_init
#define pal_mutex_destroy ps3dk_prof__pal_mutex_destroy
#define pal_mutex_lock ps3dk_prof__pal_mutex_lock
#define pal_mutex_unlock ps3dk_prof__pal_mutex_unlock
#define pal_owner_get ps3dk_prof__pal_owner_get
#define pal_owner_is_self ps3dk_prof__pal_owner_is_self
#define pal_thread_create ps3dk_prof__pal_thread_create
#define pal_thread_join ps3dk_prof__pal_thread_join
#define pal_tb ps3dk_prof__pal_tb
#define pal_tb_freq ps3dk_prof__pal_tb_freq
#define pal_mono_now_us ps3dk_prof__pal_mono_now_us
#define pal_sleep_us ps3dk_prof__pal_sleep_us
#define pal_os_thread_id ps3dk_prof__pal_os_thread_id
#define pal_open_excl ps3dk_prof__pal_open_excl
#define pal_write ps3dk_prof__pal_write
#define pal_close ps3dk_prof__pal_close

#if defined(PS3DK_PROF_PAL_HOST)
#include <pthread.h>
typedef pthread_mutex_t pal_mutex;
typedef pthread_t pal_thread;
typedef struct pal_owner {
    pthread_t t;   /* opaque: compared only with pthread_equal */
} pal_owner;
#define PAL_DEFAULT_SINK_DIR "."   /* the capture directory when no sink is set */
#else
#error "ps3dk prof: only the host PAL exists in this step (define PS3DK_PROF_PAL_HOST)"
#endif

/* Memory: one 128-byte-aligned block at init; pal_free only unwinds a failed init. */
void *pal_alloc(size_t bytes);
void pal_free(void *p);

/* Cold-path mutexes. */
int pal_mutex_init(pal_mutex *m);
int pal_mutex_destroy(pal_mutex *m);
int pal_mutex_lock(pal_mutex *m);
int pal_mutex_unlock(pal_mutex *m);

/* Thread identity.  pal_owner_is_self returns 1 only when the current identity is read successfully AND equals o. */
int pal_owner_get(pal_owner *o);
int pal_owner_is_self(const pal_owner *o);

/* The drain thread (THREAD mode). */
int pal_thread_create(pal_thread *t, void *(*fn)(void *), void *arg, size_t stack_bytes);
int pal_thread_join(pal_thread t);

/* Clocks. */
uint64_t pal_tb(void);          /* the raw timebase */
uint64_t pal_tb_freq(void);     /* 0 means unavailable */
int pal_mono_now_us(uint64_t *us);
void pal_sleep_us(uint64_t us);

/* The OS thread id recorded in THREADS (informational). */
uint64_t pal_os_thread_id(void);

/* File sink.  Each returns 0 or a negative value and sets *err to the errno-class code on failure. */
int pal_open_excl(const char *path, int *fd, int *err);
long pal_write(int fd, const void *buf, unsigned long n, int *err);   /* bytes written, or -1 */
int pal_close(int fd, int *err);
#define PAL_EINTR 4
#define PAL_EEXIST 17

#endif /* PS3DK_PROF_PAL_H */
