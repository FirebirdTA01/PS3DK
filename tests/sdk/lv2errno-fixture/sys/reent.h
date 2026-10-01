/* Host stand-in: only the errno slot of newlib's struct _reent. */
#ifndef LV2ERRNO_FIXTURE_SYS_REENT_H
#define LV2ERRNO_FIXTURE_SYS_REENT_H
struct _reent { int _errno; };
#endif
