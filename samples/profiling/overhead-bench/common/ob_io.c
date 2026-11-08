#include "ob_io.h"

#include <stdio.h>
#include <string.h>

int ob_fw_open(ob_fw *w, const ob_fio *io, const char *path)
{
    memset(w, 0, sizeof *w);
    w->io = io;
    w->fd = io->open_trunc(io->ctx, path);
    if (w->fd < 0)
        w->err = OB_IO_EOPEN;
    return w->err;
}

int ob_fw_printf(ob_fw *w, const char *fmt, ...)
{
    char buf[512];
    if (w->err)
        return w->err;
    if (w->fd < 0 || w->finished)
        return w->err = OB_IO_ENOTOPEN;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (unsigned)n >= sizeof buf)
        return w->err = OB_IO_EFMT;
    unsigned long off = 0;
    while (off < (unsigned long)n) { /* a short write continues; a failed or zero write latches */
        long k = w->io->write(w->io->ctx, w->fd, buf + off, (unsigned long)n - off);
        if (k <= 0)
            return w->err = OB_IO_EWRITE;
        off += (unsigned long)k;
    }
    w->bytes += off;
    for (int i = 0; i < n; ++i)
        w->lines += buf[i] == '\n';
    return 0;
}

int ob_fw_finish(ob_fw *w)
{
    if (w->finished)
        return w->err;
    w->finished = 1;
    if (w->fd < 0)
        return w->err ? w->err : OB_IO_ENOTOPEN;
    int s = w->io->fsync(w->io->ctx, w->fd);
    int c = w->io->close(w->io->ctx, w->fd); /* always, even after an earlier error */
    w->fd = -1;
    if (!w->err && s)
        w->err = OB_IO_ESYNC;
    if (!w->err && c)
        w->err = OB_IO_ECLOSE;
    return w->err;
}

int ob_settle(const ob_fio *io, int ok, const char *tmp, const char *final_path, const char *failed)
{
    if (ok == 0)
        return io->rename(io->ctx, tmp, final_path) == 0 ? 0 : OB_IO_ERENAME;
    (void)io->rename(io->ctx, tmp, failed); /* best effort: the failed evidence apart from the accepted name */
    return ok;
}

int ob_publish_text(const ob_fio *io, const char *tmp, const char *final_path, const char *failed, const char *text)
{
    ob_fw w;
    int rc = ob_fw_open(&w, io, tmp);
    if (rc == 0)
        rc = ob_fw_printf(&w, "%s", text);
    int f = ob_fw_finish(&w);
    if (!rc)
        rc = f;
    if (w.err == OB_IO_EOPEN)
        return rc; /* nothing was created */
    return ob_settle(io, rc, tmp, final_path, failed);
}
