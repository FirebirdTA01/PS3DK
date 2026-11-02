/* Host-test syscall seam for the launch-marker probe (included by source/main.c only under LM_HOST_TEST, after the
 * system headers).  Faults come from LM_FAULTS, every call is traced to LM_TRACE (lm_seam.c). */
#ifndef LM_SEAM_H
#define LM_SEAM_H
#include <stddef.h>
#include <sys/types.h>

int lm_seam_open(const char *path, int flags, int mode);
ssize_t lm_seam_write(int fd, const void *buf, size_t n);
int lm_seam_fsync(int fd);
int lm_seam_close(int fd);

#define open lm_seam_open
#define write lm_seam_write
#define fsync lm_seam_fsync
#define close lm_seam_close
#endif
