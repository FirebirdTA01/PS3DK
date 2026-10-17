/* Host stand-in for <sys/lv2errno.h>: the three translator prototypes. */
#ifndef LV2ERRNO_FIXTURE_SYS_LV2ERRNO_H
#define LV2ERRNO_FIXTURE_SYS_LV2ERRNO_H
#include <ppu-types.h>
struct _reent;
s32 lv2error(s32 error);
s32 lv2errno(s32 error);
s32 lv2errno_r(struct _reent *r, s32 error);
#endif
