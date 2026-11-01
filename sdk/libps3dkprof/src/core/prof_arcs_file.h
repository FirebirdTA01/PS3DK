/* Private candidate-file adapter. No installed archive or startup wiring. */
#ifndef PS3DK_PROF_ARCS_FILE_H
#define PS3DK_PROF_ARCS_FILE_H
#include "prof_arcs_export.h"

/* OS-style callbacks: control operations return 0 on success, nonzero with
 * errno-class *err on failure. Failed open transfers no descriptor ownership;
 * successful open gives a nonnegative fd. Read zero is EOF; write zero fails.
 * They may return a positive short count, never write beyond the request, and
 * may have partial side effects before error. The adapter does not retry errors
 * except EINTR (8 retries for open/read/write/rewind/sync, never for close). */
struct ps3dk_prof_file_ops {
    int (*open)(void *, const char *, int read_write, int *fd, int *err);
    long (*read)(void *, int, void *, unsigned long, int *err);
    long (*write)(void *, int, const void *, unsigned long, int *err);
    int (*rewind)(void *, int, int *err);
    int (*sync)(void *, int, int *err);
    int (*close)(void *, int, int *err);
};
enum ps3dk_prof_file_operation {
    PS3DK_PROF_FILE_NONE, PS3DK_PROF_FILE_OPEN_SPOOL, PS3DK_PROF_FILE_WRITE_SPOOL,
    PS3DK_PROF_FILE_SYNC_SPOOL, PS3DK_PROF_FILE_REWIND, PS3DK_PROF_FILE_OPEN_OUTPUT,
    PS3DK_PROF_FILE_READ, PS3DK_PROF_FILE_WRITE_OUTPUT, PS3DK_PROF_FILE_SYNC_OUTPUT,
    PS3DK_PROF_FILE_CLOSE_OUTPUT, PS3DK_PROF_FILE_CLOSE_SPOOL
};
struct ps3dk_prof_arc_file {
    struct ps3dk_prof_file_ops ops;
    void *ctx;
    char spool_path[1024], pending_path[1024];
    int initialized, phase, busy, spool_fd, output_fd;
    int error, os_error;
    enum ps3dk_prof_file_operation operation;
};
/* Zeroed, caller-owned state. Single owner: sole drainer writes, then shutdown
 * joins it before prepare/replay. No callback may modify the state or its ops.
 * ops==NULL selects the PAL. Token: 1..64 ASCII alnum/_/-. Directory is trusted
 * configuration and must exist; trailing '/' is normalized, retaining root.
 * Both names are exclusive and retained forever
 * by this adapter; it never creates a final .arcs name, renames or deletes.
 * Invalid args leave state untouched; a valid failed init is terminal.
 * Each OS read/write <=65536 bytes; at most 8 consecutive EINTR retries, reset
 * on progress. Individual syscalls may block; this is not a time bound. */
int ps3dk_prof__file_init(struct ps3dk_prof_arc_file *, const char *directory,
                        const char *token, const struct ps3dk_prof_file_ops *, void *ctx);
int ps3dk_prof__file_spool_write(void *, const void *, size_t);
/* One shot, only after stream_finish and runtime shutdown/join: sync spool,
 * rewind the SAME rw descriptor, open .arcs.pending exclusively. Failure closes
 * all owned descriptors once. No pathname reopen or failed-session retry. */
int ps3dk_prof__file_prepare(struct ps3dk_prof_arc_file *);
ptrdiff_t ps3dk_prof__file_read(void *, void *, size_t);
int ps3dk_prof__file_output_write(void *, const void *, size_t);
int ps3dk_prof__file_output_flush(void *);
int ps3dk_prof__file_output_close(void *);
/* The coordinator closes output only after admission. On preflight refusal the
 * caller must cleanup. Read/write/flush errors latch failure without closing
 * output early. Close drops ownership BEFORE the call and never retries, even
 * EINTR. Cleanup remains usable after error, rejects callback reentry, closes
 * remaining owned descriptors once, and repeats the first failure forever.
 * A successful candidate remains .arcs.pending; even bytes that parse COMPLETE
 * are not published, and must not override a close/cleanup failure. */
int ps3dk_prof__file_cleanup(struct ps3dk_prof_arc_file *);
#endif
