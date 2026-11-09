/* Ordinary pthread calls, with capture lifecycle only: no manual zones or wait hooks.
 * The external run harness bounds a hung LV2 call/join; no recovery is claimed here. */
#include <ps3dk/prof.h>
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { BLOCKS = 64, PER_BLOCK = 32, TIMED_BLOCKS = 2, COND_CALLS = 16, MAX_ROWS = 84 };
typedef struct row { const char *kind; unsigned calls; uint64_t begin, end; } row;
static row rows[MAX_ROWS];
static unsigned row_count;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t timed_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static unsigned ready;
static unsigned worker_entered;
static unsigned timed_held, timed_done;
static int worker_rc;
static unsigned locks, timed_locks, conditions, worker_locks;

static uint64_t tick(void) {
    __asm__ volatile("isync" ::: "memory");
    uint64_t t = __builtin_ppc_get_timebase();
    __asm__ volatile("isync" ::: "memory");
    return t;
}
static int deadline(struct timespec *t, long ms) {
    if (clock_gettime(CLOCK_REALTIME, t)) return 1;
    t->tv_nsec += ms * 1000000L;
    if (t->tv_nsec >= 1000000000L) { ++t->tv_sec; t->tv_nsec -= 1000000000L; }
    return 0;
}
static int record(const char *kind, unsigned calls, uint64_t a, uint64_t b) {
    if (row_count == MAX_ROWS || b < a) return 1;
    rows[row_count++] = (row){kind, calls, a, b};
    return 0;
}
static void *worker(void *unused) {
    (void)unused;
    /* The runtime's start hook has returned before this publication. */
    __atomic_store_n(&worker_entered, 1, __ATOMIC_RELEASE);
    /* Created before init: the existing explicit registration API admits it. */
    for (unsigned i = 0; !__atomic_load_n(&ready, __ATOMIC_ACQUIRE); ++i) {
        if (i == 5000) { worker_rc = 1; return NULL; }
        usleep(1000);
    }
    worker_rc = ps3dk_prof_thread_begin("preexisting-worker");
    if (worker_rc) return NULL;
    for (unsigned i = 0; i < PER_BLOCK; ++i) {
        int rc = pthread_mutex_lock(&mutex);
        if (rc) { worker_rc = rc; break; }
        ++worker_locks;
        rc = pthread_mutex_unlock(&mutex);
        if (rc) { worker_rc = rc; break; }
    }
    if (worker_rc) return NULL;
    /* Setup uses trylock, outside the three observed operation kinds. The
     * worker owns this distinct mutex until all main-thread timeouts finish. */
    worker_rc = pthread_mutex_trylock(&timed_mutex);
    if (worker_rc) return NULL;
    __atomic_store_n(&timed_held, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&timed_done, __ATOMIC_ACQUIRE)) usleep(1000);
    worker_rc = pthread_mutex_unlock(&timed_mutex);
#if !WAIT_OBSERVED
    /* The default runtime has no observed lifecycle adapter. */
    ps3dk_prof_thread_end();
#endif
    return NULL;
}

static int await_worker_entered(void) {
    for (unsigned i = 0; !__atomic_load_n(&worker_entered, __ATOMIC_ACQUIRE); ++i) {
        if (i == 5000) return 1;
        usleep(1000);
    }
    return 0;
}

static int await_timed_holder(void) {
    for (unsigned i = 0; !__atomic_load_n(&timed_held, __ATOMIC_ACQUIRE); ++i) {
        if (i == 5000) return 1;
        usleep(1000);
    }
    return 0;
}

static int exercise(void) {
    /* Warm the static mutex separately; every successful call is counted. */
    if (pthread_mutex_lock(&mutex)) return 1;
    ++locks;
    if (pthread_mutex_unlock(&mutex) || ps3dk_prof_drain()) return 1;
    for (unsigned block = 0; block < BLOCKS; ++block) {
        uint64_t a = tick();
        for (unsigned i = 0; i < PER_BLOCK; ++i) {
            if (pthread_mutex_lock(&mutex)) return 1;
            ++locks;
            if (pthread_mutex_unlock(&mutex)) return 1;
        }
        uint64_t b = tick();
        if (record("mutex", PER_BLOCK, a, b) || ps3dk_prof_drain()) return 1;
    }
    for (unsigned block = 0; block < TIMED_BLOCKS; ++block) {
        uint64_t a = tick();
        for (unsigned i = 0; i < PER_BLOCK; ++i) {
            struct timespec until;
            if (deadline(&until, 1)) return 1;
            if (pthread_mutex_timedlock(&timed_mutex, &until) != ETIMEDOUT) return 1;
            ++timed_locks;
        }
        uint64_t b = tick();
        if (record("timed_mutex", PER_BLOCK, a, b) || ps3dk_prof_drain()) return 1;
    }
    __atomic_store_n(&timed_done, 1, __ATOMIC_RELEASE);
    if (pthread_mutex_lock(&mutex)) return 1;
    ++locks;
    for (unsigned i = 0; i < COND_CALLS; ++i) {
        struct timespec until;
        if (deadline(&until, 20)) return 1;
        uint64_t a = tick();
        int rc = pthread_cond_timedwait(&cond, &mutex, &until);
        uint64_t b = tick();
        ++conditions;
        /* An early/spurious success fails this exact-count control; no retry. */
        if (rc != ETIMEDOUT || record("condition_timeout", 1, a, b)) return 1;
    }
    if (pthread_mutex_unlock(&mutex) || ps3dk_prof_drain()) return 1;
    return 0;
}

static int receipt(const ps3dk_prof_capture_record *capture, int shutdown_rc) {
    char temp[300], path[300];
    int n = snprintf(path, sizeof path, "%s.wait.txt", capture->path);
    if (n < 0 || (size_t)n >= sizeof path) return 1;
    n = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof temp) return 1;
    FILE *f = fopen(temp, "wb");
    if (!f) return 1;
    int failed = fprintf(f, "WAIT_SAMPLE schema=1 arm=%s build=%s session=%016" PRIx64 "\n"
        "COUNTS mutex=%u timed_mutex=%u condition_timeout=%u worker_mutex=%u rows=%u\n"
        "SHUTDOWN rc=%d\n", WAIT_OBSERVED ? "ON" : "OFF", WAIT_BUILD_ID,
        capture->session_id, locks, timed_locks, conditions, worker_locks, row_count, shutdown_rc) < 0;
    for (unsigned i = 0; i < row_count; ++i)
        if (fprintf(f, "ROW index=%u kind=%s calls=%u begin=%" PRIu64 " end=%" PRIu64 "\n",
            i, rows[i].kind, rows[i].calls, rows[i].begin, rows[i].end) < 0) failed = 1;
    if (fprintf(f, "WAIT_SAMPLE_END complete=1\n") < 0) failed = 1;
    if (fflush(f)) failed = 1;
    if (fsync(fileno(f))) failed = 1;
    if (fclose(f)) failed = 1;
    if (failed) return 1;
    return rename(temp, path) != 0;
}

int main(void) {
    pthread_t thread;
    ps3dk_prof_config cfg;
    ps3dk_prof_capture_record capture;
    ps3dk_prof_config_defaults(&cfg);
    cfg.mode = PS3DK_PROF_MODE_APP;
    cfg.max_threads = 64;
    cfg.max_descriptors = 16;
    cfg.ring_records = 512;
    cfg.app = "wait-observations";
    cfg.build_id = WAIT_BUILD_ID;
    cfg.workload_tag = WAIT_OBSERVED ? "ON" : "OFF";
    /* On any early failure the process owns all static storage until exit.
     * Do not destroy or shut down underneath a possibly live producer. */
    if (pthread_create(&thread, NULL, worker, NULL)) return 2;
    if (await_worker_entered()) return 2;
    if (ps3dk_prof_set_sink_path("/dev_hdd0/tmp") || ps3dk_prof_init(&cfg)) return 2;
    if (ps3dk_prof_get_capture_record(&capture)) return 2;
#if WAIT_OBSERVED
    /* The observed init path already registered this caller as "init". */
    if (ps3dk_prof_thread_begin("init") != PS3DK_PROF_EALREADY) return 2;
#else
    if (ps3dk_prof_thread_begin("main")) return 2;
#endif
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    if (await_timed_holder()) return 2;
    int exercise_failed = ps3dk_prof_drain() || exercise();
    __atomic_store_n(&timed_done, 1, __ATOMIC_RELEASE); /* Also release on exercise failure. */
    if (exercise_failed) return 2;
    if (pthread_join(thread, NULL) || worker_rc || worker_locks != PER_BLOCK) return 2;
    if (locks != 2050 || timed_locks != 64 || conditions != 16 || row_count != 82) return 2;
    if (pthread_cond_destroy(&cond) || pthread_mutex_destroy(&mutex) ||
        pthread_mutex_destroy(&timed_mutex)) return 2;
    ps3dk_prof_thread_end();
    if (ps3dk_prof_drain()) return 2;
    int rc = ps3dk_prof_shutdown();
    if (rc || receipt(&capture, rc)) return 2;
    printf("WAIT_SAMPLE_OK arm=%s session=%016" PRIx64 " path=%s\n",
           WAIT_OBSERVED ? "ON" : "OFF", capture.session_id, capture.path);
    return 0;
}
