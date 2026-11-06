/* Live link hardware gate 0: can an app thread read the profiler's capture
 * file while the profiler's file sink still has it open, and does what it
 * reads match the final file?
 *
 * - This run creates its own directory, /dev_hdd0/tmp/liveprobe-<t0>,
 *   exclusively, before profiler init, and points the file sink there, so
 *   the only capture in it is this run's.
 * - The main thread profiles ~6 s of zones, draining every frame. A second
 *   thread opens the capture read-only while the sink has it open and
 *   reads new bytes every 100 ms, noting each size and whether observation
 *   was still ELIGIBLE when that read completed. Main clears eligibility
 *   BEFORE it calls shutdown, so only bytes read before shutdown began
 *   count as "seen while open".
 * - After shutdown the reader drains the rest; main reads the closed file
 *   fresh, to EOF, and compares (probe_oracle.h decides).
 * Output: TTY lines PROBE_* and <dir>/liveprobe-result.txt. */
#include "probe_oracle.h"
#include <ps3dk/prof.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

SYS_PROCESS_PARAM(1001, 0x100000);

/* PROBE_CONTROL=1: the matched workload control, no reader: the same
 * profiled workload and file, without the reader thread. */
#ifndef PROBE_CONTROL
#define PROBE_CONTROL 0
#endif

#define BASE_DIR "/dev_hdd0/tmp"
#define CAP_BYTES (8u << 20)

static uint64_t g_t0;
static char g_dir[128], g_capture[384];
static atomic_int g_stop, g_eligible;
static struct probe_reader g_reader;
static int g_open_errno;

static uint64_t now_us(void)
{
    return sys_time_get_system_time();
}

static ssize_t fd_read(void *ctx, void *buf, size_t n, uint64_t off)
{
    int fd = *(int *)ctx;
    if (lseek(fd, (off_t)off, SEEK_SET) < 0)
        return errno ? -errno : -1;
    ssize_t r = read(fd, buf, n);
    return r < 0 ? (errno ? -errno : -1) : r;
}

/* The one .prof in this run's own directory. */
static int find_capture(void)
{
    DIR *d = opendir(g_dir);
    if (!d)
        return -1;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 5 && strcmp(e->d_name + len - 5, ".prof") == 0) {
            snprintf(g_capture, sizeof g_capture, "%s/%s", g_dir, e->d_name);
            ++found;
        }
    }
    closedir(d);
    return found == 1 ? 0 : -1;
}

static void *reader(void *arg)
{
    (void)arg;
    int fd = -1;
    while (!atomic_load(&g_stop)) {
        if (fd < 0 && find_capture() == 0) {
            fd = open(g_capture, O_RDONLY);
            if (fd < 0)
                g_open_errno = errno ? errno : -1;
        }
        if (fd >= 0) {
            probe_read_available(&g_reader.kept, fd_read, &fd);
            /* Eligibility as it stands now that the read has completed. */
            int eligible = atomic_load(&g_eligible);
            probe_note(&g_reader, now_us(), eligible);
        }
        usleep(100000);
    }
    if (fd >= 0) {
        probe_read_available(&g_reader.kept, fd_read, &fd); /* the rest, after close */
        probe_note(&g_reader, now_us(), 0);
        close(fd);
    }
    return NULL;
}

static void busy_us(uint64_t us)
{
    uint64_t t = now_us();
    volatile uint32_t x = 1;
    while (now_us() - t < us)
        x = x * 1664525u + 1013904223u;
}

int main(void)
{
    printf("PROBE_BEGIN live-read gate 0\n");
    g_t0 = now_us(); /* the common epoch, before any thread exists */
    snprintf(g_dir, sizeof g_dir, "%s/liveprobe-%016llx", BASE_DIR, (unsigned long long)g_t0);
    if (mkdir(g_dir, 0700) != 0) { /* exclusive: an existing directory fails */
        printf("PROBE_RESULT FAIL could not create %s errno=%d\n", g_dir, errno);
        return 1;
    }
    unsigned char *kept = malloc(CAP_BYTES + 1), *final = malloc(CAP_BYTES + 1);
    if (!kept || !final) {
        puts("PROBE_RESULT FAIL no memory");
        return 1;
    }
    g_reader.kept = (struct probe_buf){.buf = kept, .cap = CAP_BYTES};

    ps3dk_prof_config cfg;
    ps3dk_prof_config_defaults(&cfg);
    cfg.app = "liveprobe";
    cfg.build_id = "live-read-probe";
    cfg.workload_tag = "gate0";
    cfg.mode = PS3DK_PROF_MODE_APP;
    cfg.max_threads = 1;
    cfg.max_descriptors = 8;
    cfg.ring_records = 512;
    if (ps3dk_prof_set_sink_path(g_dir) || ps3dk_prof_init(&cfg) || ps3dk_prof_thread_begin("main")) {
        puts("PROBE_RESULT FAIL profiler init");
        return 1;
    }
    atomic_store(&g_eligible, 1);
    pthread_t t;
    if (!PROBE_CONTROL && pthread_create(&t, NULL, reader, NULL) != 0) {
        puts("PROBE_RESULT FAIL reader thread");
        ps3dk_prof_shutdown();
        return 1;
    }
    int drain_failures = 0;
    for (int f = 0; f < 300; ++f) {
        ps3dk_prof_frame();
        {
            PS3DK_PROF_ZONE("probe.work");
            busy_us(8000);
        }
        {
            PS3DK_PROF_ZONE("probe.wait");
            usleep(10000);
        }
        if (ps3dk_prof_drain() != 0)
            ++drain_failures;
    }
    /* Observations from here on are not "seen while open". */
    atomic_store(&g_eligible, 0);
    uint64_t shutdown_begin = now_us();
    int shut = ps3dk_prof_shutdown();
    uint64_t shutdown_end = now_us();
    usleep(300000);
    atomic_store(&g_stop, 1);
    int join_rc = PROBE_CONTROL ? 0 : pthread_join(t, NULL);
    int joined = join_rc == 0;

    /* The final file, read fresh, to EOF. */
    struct probe_buf fin = {.buf = final, .cap = CAP_BYTES};
    int final_eof = 0;
    /* No reader thread in the control: main finds the capture itself, and
     * opens nothing unless exactly one capture is in this run's directory. */
    int capture_found = joined && (PROBE_CONTROL ? find_capture() == 0 : g_capture[0] != 0);
    int ffd = joined && capture_found ? open(g_capture, O_RDONLY) : -1;
    if (ffd >= 0) {
        final_eof = probe_read_available(&fin, fd_read, &ffd);
        close(ffd);
    } else {
        fin.err = joined ? (errno ? errno : -1) : 0;
    }
    struct probe_verdict v;
    if (!joined)
        v = (struct probe_verdict){0, "reader thread not joined"};
    else if (PROBE_CONTROL)
        /* The control has no reader: it only shows the same app ran to the
         * end on this console and wrote a complete, readable file. */
        v = (struct probe_verdict){shut == 0 && !drain_failures && !fin.err && !fin.cap_hit && final_eof &&
                                       fin.len > 0,
                                   "control: same workload, no reader thread"};
    else
        v = probe_judge(&g_reader, &fin, final_eof, shut, drain_failures, joined);

    char rpath[192];
    snprintf(rpath, sizeof rpath, "%s/liveprobe-result.txt", g_dir);
    FILE *out = fopen(rpath, "w");
    for (int k = 0; k < 2; ++k) {
        FILE *o = k ? out : stdout;
        if (!o)
            continue;
        /* Main-owned data first; reader-owned state only after a join. */
        fprintf(o, "PROBE_MODE %s\n", PROBE_CONTROL ? "control (no reader thread)" : "probe");
        fprintf(o, "PROBE_DIR %s\n", g_dir);
        fprintf(o, "PROBE_JOIN rc=%d\n", join_rc);
        fprintf(o, "PROBE_SHUTDOWN begin_ms=%llu end_ms=%llu rc=%d drain_failures=%d\n",
                (unsigned long long)((shutdown_begin - g_t0) / 1000),
                (unsigned long long)((shutdown_end - g_t0) / 1000), shut, drain_failures);
        if (joined && !PROBE_CONTROL) {
            fprintf(o, "PROBE_CAPTURE %s\n", g_capture);
            for (int i = 0; i < g_reader.nobs; ++i)
                fprintf(o, "PROBE_OBS t_ms=%llu size=%llu eligible=%d\n",
                        (unsigned long long)((g_reader.obs[i].t_us - g_t0) / 1000),
                        (unsigned long long)g_reader.obs[i].size, g_reader.obs[i].eligible);
            fprintf(o, "PROBE_GROWTH_BEFORE_SHUTDOWN %d\n", probe_growth_eligible(&g_reader));
            fprintf(o, "PROBE_READER len=%llu err=%d cap_hit=%d open_errno=%d\n",
                    (unsigned long long)g_reader.kept.len, g_reader.kept.err, g_reader.kept.cap_hit,
                    g_open_errno);
        } else if (!joined) {
            fprintf(o, "PROBE_READER unavailable: join failed\n");
        } else {
            fprintf(o, "PROBE_CAPTURE %s\n", capture_found ? g_capture : "(not exactly one capture found)");
        }
        fprintf(o, "PROBE_FINAL len=%llu err=%d cap_hit=%d eof=%d\n", (unsigned long long)fin.len, fin.err,
                fin.cap_hit, final_eof);
        fprintf(o, "PROBE_RESULT %s %s\n", v.pass ? "PASS" : "FAIL", v.why);
    }
    if (out)
        fclose(out);
    if (joined) {
        free(kept);
        free(final);
    }
    return v.pass ? 0 : 1;
}
