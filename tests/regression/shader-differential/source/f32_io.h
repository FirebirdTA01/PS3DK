/* Float-mode artifact write, with every failure reported: the open, a
 * short write and the close.  The stdio calls go through F32_FOPEN /
 * F32_FWRITE / F32_FCLOSE so the host test (f32-gate-test.c) can inject
 * each failure; the guest uses plain stdio.  A failed write latches the
 * run INVALID in run_f32 (codex review 2026-10-19 20:12). */
#ifndef F32_IO_H
#define F32_IO_H

#include <stdio.h>

#ifndef F32_FOPEN
#define F32_FOPEN  fopen
#define F32_FWRITE fwrite
#define F32_FCLOSE fclose
#endif

/* 1 when all n bytes reached the file and it closed cleanly, else 0. */
static int f32_write_file(const char *path, const void *buf, size_t n)
{
	FILE *f = F32_FOPEN(path, "wb");
	if (!f)
		return 0;
	size_t w = F32_FWRITE(buf, 1, n, f);
	int closed = F32_FCLOSE(f) == 0;
	return closed && w == n;
}

#endif
