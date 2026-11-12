/*
 * samples/gcm/common/gcm_segment_crumb.h
 *
 * Progress crumbs for the command-segment samples.  Each step prints a
 * GCM_SEG_STEP line and appends the same line to
 * /dev_hdd0/tmp/gcmseg-crumb.txt with lv2 file syscalls, synced after every
 * line, so the last step reached can be read over FTP even if the console
 * locks up.  The file is truncated once, when the sample starts.
 */
#ifndef PS3TC_SAMPLES_GCM_SEGMENT_CRUMB_H
#define PS3TC_SAMPLES_GCM_SEGMENT_CRUMB_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>

#define SEG_CRUMB_PATH "/dev_hdd0/tmp/gcmseg-crumb.txt"
#ifndef SYS_O_WRONLY
#define SYS_O_WRONLY 000001
#endif
#ifndef SYS_O_CREAT
#define SYS_O_CREAT  000100
#endif
#ifndef SYS_O_TRUNC
#define SYS_O_TRUNC  001000
#endif

static s32 seg_crumb_fd = -1;

static inline void seg_crumb_open(const char *sample)
{
    if (sysLv2FsOpen(SEG_CRUMB_PATH, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &seg_crumb_fd,
                     0666, NULL, 0) != 0)
        seg_crumb_fd = -1;
    printf("GCM_SEG_STEP crumb_file=%s open=%s sample=%s\n", SEG_CRUMB_PATH,
           seg_crumb_fd >= 0 ? "ok" : "FAILED", sample);
    fflush(stdout);
}

__attribute__((format(printf, 1, 2)))
static inline void seg_step(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    int n = snprintf(line, sizeof(line), "GCM_SEG_STEP ");
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    line[len++] = '\n';
    fwrite(line, 1, len, stdout);
    fflush(stdout);
    if (seg_crumb_fd >= 0) {
        u64 written = 0;
        sysLv2FsWrite(seg_crumb_fd, line, len, &written);
        sysLv2FsFsync(seg_crumb_fd);
    }
}

static inline void seg_crumb_close(void)
{
    if (seg_crumb_fd >= 0) { sysLv2FsClose(seg_crumb_fd); seg_crumb_fd = -1; }
}

#endif
