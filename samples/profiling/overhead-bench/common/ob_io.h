/* overhead-bench checked file output (portable C over a file port; the target binds POSIX open/write/fsync/close/
 * rename, the host controls a mock that fails any chosen call).
 *
 * ob_fw: one output file written through a sticky-error buffer.  ob_fw_open opens <path>; every ob_fw_printf
 * formats into the buffer and writes it out (a short or failed write, or a formatting overflow, latches the first
 * error; later calls do nothing and return it).  ob_fw_finish ALWAYS runs fsync and close when the file was opened
 * (close even after an earlier error) and returns the first error of the whole file.  No call is skipped by an
 * earlier failure except writes after the latch.
 *
 * Publication: a result is written to <tmp>, finished, and only when every step succeeded renamed to <final>;
 * otherwise the tmp is renamed to <failed> (the failed evidence is kept apart, never under the accepted name).  A
 * failed rename to <final> leaves the tmp in place (reported).  ob_publish_text does the whole sequence for one
 * short text (the receipt). */
#ifndef OB_IO_H
#define OB_IO_H

#include <stdarg.h>
#include <stdint.h>

typedef struct ob_fio {
    void *ctx;
    int (*open_trunc)(void *ctx, const char *path);                    /* fd >= 0, or < 0 */
    long (*write)(void *ctx, int fd, const void *buf, unsigned long n); /* bytes written, or < 0 */
    int (*fsync)(void *ctx, int fd);
    int (*close)(void *ctx, int fd);
    int (*rename)(void *ctx, const char *from, const char *to);
} ob_fio;

enum { OB_IO_OK = 0, OB_IO_EOPEN = -1, OB_IO_EWRITE = -2, OB_IO_EFMT = -3, OB_IO_ESYNC = -4, OB_IO_ECLOSE = -5,
       OB_IO_ERENAME = -6, OB_IO_ENOTOPEN = -7 };

typedef struct ob_fw {
    const ob_fio *io;
    int fd, err, finished;
    unsigned long bytes, lines;
} ob_fw;

int ob_fw_open(ob_fw *w, const ob_fio *io, const char *path);
int ob_fw_printf(ob_fw *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int ob_fw_finish(ob_fw *w); /* fsync + close (both always tried when opened); the first error */

/* rename tmp -> final when ok == 0, else tmp -> failed.  Returns 0 only when ok == 0 and the rename to final
 * succeeded. */
int ob_settle(const ob_fio *io, int ok, const char *tmp, const char *final_path, const char *failed);
int ob_publish_text(const ob_fio *io, const char *tmp, const char *final_path, const char *failed, const char *text);

#endif
