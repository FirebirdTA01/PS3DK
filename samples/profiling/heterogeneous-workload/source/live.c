/* The live link in this sample (SAMPLE_LIVE): the PC viewer watches the capture as it grows. See ps3dk/prof.h for
 * the API contract and README.md for the viewer side.
 *
 * Every network and live result is recorded, one "key=value ..." line each, in an owned run file next to the CSV
 * (heterogeneous-<id>.live.txt), so the stop and error paths can be read after the console returns to the XMB:
 *   net_init rc=..                       live_start rc=.. port=.. ip=..
 *   stop attempt=N graceful_us=.. join_us=.. rc=..
 *   stats rc=0 version=1 connections=.. busy_refusals=.. bytes_delivered=.. pongs=.. drop_<reason>=.. (all ten)
 *     or stats rc=<refusal> (no counters)
 *   net_deinit context=.. rc=..          end network_left_initialized=0|1 live_stopped=0|1
 * The record is separate from the workload result and the profiler's verdict. A failure to write it is printed. */
#include "live.h"
#include <ps3dk/prof.h>
#include <cell/libnetctl.h>
#include <cell/sysmodule.h>
#include <net/net.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int net_up, live_up, live_stopped, log_failed;
static FILE *live_log;

static void record(const char *fmt, ...)
{
    va_list ap;
    if (!live_log)
        return;
    va_start(ap, fmt);
    int w = vfprintf(live_log, fmt, ap);
    va_end(ap);
    if (w < 0 || fputc('\n', live_log) == EOF || fflush(live_log) != 0)
        log_failed = 1;
}

static void open_record(const char *csv_path)
{
    char path[192];
    size_t n = strlen(csv_path);
    if (n < 4 || strcmp(csv_path + n - 4, ".csv") != 0 ||
        snprintf(path, sizeof path, "%.*s.live.txt", (int)(n - 4), csv_path) >= (int)sizeof path) {
        puts("WORKLOAD_LIVE record-path failed");
        log_failed = 1;
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    live_log = fd < 0 ? NULL : fdopen(fd, "w");
    if (!live_log) {
        if (fd >= 0)
            close(fd);
        printf("WORKLOAD_LIVE record-open failed path=%s\n", path);
        log_failed = 1;
        return;
    }
    printf("WORKLOAD_LIVE record=%s\n", path);
}

static void close_record(void)
{
    if (!live_log)
        return;
    if (fflush(live_log) != 0 || fsync(fileno(live_log)) != 0)
        log_failed = 1;
    if (fclose(live_log) != 0)
        log_failed = 1;
    live_log = NULL;
    if (log_failed)
        puts("WORKLOAD_LIVE record-write failed: the live record is incomplete");
}

static void net_down(const char *context)
{
    int d = netDeinitialize();
    record("net_deinit context=%s rc=%d", context, d);
    printf("WORKLOAD_LIVE net-deinit context=%s rc=%d\n", context, d);
    if (d == 0)
        net_up = 0;
}

/* The console's IPv4 address for the screen, or "this console" if netctl cannot say. */
static void console_ip(char *out, size_t n)
{
    snprintf(out, n, "this console");
    if (cellSysmoduleLoadModule(CELL_SYSMODULE_NETCTL) != 0)
        return;
    if (cellNetCtlInit() == 0) {
        union CellNetCtlInfo info;
        memset(&info, 0, sizeof info);
        if (cellNetCtlGetInfo(CELL_NET_CTL_INFO_IP_ADDRESS, &info) == 0 && info.ip_address[0])
            snprintf(out, n, "%.15s", info.ip_address);
        cellNetCtlTerm();
    }
    cellSysmoduleUnloadModule(CELL_SYSMODULE_NETCTL);
}

void sample_live_begin(const char *csv_path, char *status, size_t n)
{
    open_record(csv_path);
    int rc = netInitialize();
    record("net_init rc=%d", rc);
    if (rc != 0) {
        printf("WORKLOAD_LIVE net-init rc=%d\n", rc);
        snprintf(status, n, "Live view: network init failed (%d); capture continues", rc);
        return;
    }
    net_up = 1;
    ps3dk_prof_live_config cfg;
    memset(&cfg, 0, sizeof cfg); /* zero fields take the defaults */
    cfg.port = SAMPLE_LIVE_PORT;
    rc = ps3dk_prof_live_start(&cfg);
    if (rc != PS3DK_PROF_OK) {
        record("live_start rc=%d port=%u", rc, SAMPLE_LIVE_PORT);
        printf("WORKLOAD_LIVE start rc=%d\n", rc);
        snprintf(status, n, "Live view: start failed (%d); capture continues", rc);
        net_down("failed-start"); /* no agent thread exists after a failed start */
        return;
    }
    live_up = 1;
    char ip[24];
    console_ip(ip, sizeof ip);
    record("live_start rc=0 port=%u ip=%s", SAMPLE_LIVE_PORT, ip);
    printf("WORKLOAD_LIVE listening ip=%s port=%u\n", ip, SAMPLE_LIVE_PORT);
    snprintf(status, n, "Live view: connect the PC viewer to %s:%u", ip, SAMPLE_LIVE_PORT);
}

/* The agent's final counters, recorded before the network is torn down.  The rc is always recorded: only rc=0
 * carries counters, so a refused or failed copy-out is never read as zeros.  The same line also goes to the TTY: a
 * second observation channel if the record write fails (reported by close_record), not a guarantee when both fail. */
static void record_stats(void)
{
    ps3dk_prof_live_stats s;
    int rc = ps3dk_prof_live_get_stats(&s);
    if (rc != PS3DK_PROF_OK) {
        record("stats rc=%d", rc);
        printf("WORKLOAD_LIVE stats rc=%d\n", rc);
        return;
    }
    char line[512];
    int w = snprintf(line, sizeof line,
             "stats rc=0 version=%u connections=%llu busy_refusals=%llu bytes_delivered=%llu pongs=%llu "
             "drop_end=%llu drop_handshake_timeout=%llu drop_bad_hello=%llu drop_protocol=%llu drop_ping_flood=%llu "
             "drop_stalled=%llu drop_peer_closed=%llu drop_io=%llu drop_cancelled=%llu drop_clock=%llu",
             (unsigned)s.version, (unsigned long long)s.connections, (unsigned long long)s.busy_refusals,
             (unsigned long long)s.bytes_delivered, (unsigned long long)s.pongs, (unsigned long long)s.drop_end,
             (unsigned long long)s.drop_handshake_timeout, (unsigned long long)s.drop_bad_hello,
             (unsigned long long)s.drop_protocol, (unsigned long long)s.drop_ping_flood,
             (unsigned long long)s.drop_stalled, (unsigned long long)s.drop_peer_closed,
             (unsigned long long)s.drop_io, (unsigned long long)s.drop_cancelled, (unsigned long long)s.drop_clock);
    if (w < 0 || (size_t)w >= sizeof line) {
        record("stats rc=0 format-failed w=%d", w);
        printf("WORKLOAD_LIVE stats rc=0 format-failed w=%d\n", w);
        return;
    }
    record("%s", line);
    printf("WORKLOAD_LIVE %s\n", line);
}

static int stop_attempt(int attempt, uint64_t graceful_us, uint64_t join_us)
{
    int rc = ps3dk_prof_live_stop(graceful_us, join_us);
    record("stop attempt=%d graceful_us=%llu join_us=%llu rc=%d", attempt, (unsigned long long)graceful_us,
           (unsigned long long)join_us, rc);
    printf("WORKLOAD_LIVE stop attempt=%d rc=%d\n", attempt, rc);
    return rc;
}

void sample_live_end(void)
{
    if (live_up) {
        /* Nominally up to 2 s for a connected viewer to receive END, then cancel, 1 s for the thread; then 2 s more
         * if it is still stopping. Not a hard bound: an LV2 file call in progress and the final join are not. */
        int rc = stop_attempt(1, 2000000, 1000000);
        if (rc == PS3DK_PROF_ETIMEDOUT || rc == PS3DK_PROF_ECLOCK)
            rc = stop_attempt(2, 0, 2000000);
        record_stats(); /* rc=0 with counters after a successful stop; otherwise its refusal (EBUSY or ETHREAD) */
        if (rc == PS3DK_PROF_OK) {
            live_up = 0;
            live_stopped = 1;
            net_down("stopped");
        } else if (rc == PS3DK_PROF_ETHREAD) {
            puts("WORKLOAD_LIVE the agent join failed: the network stays initialized");
        } else {
            puts("WORKLOAD_LIVE the agent did not finish: the network stays initialized (process exit releases it)");
        }
    } else if (net_up) {
        net_down("no-live");
    }
    record("end network_left_initialized=%d live_stopped=%d", net_up, live_stopped);
    close_record();
}
