/*
 * PS3 Custom Toolchain — PSL1GHT-compat forwarding header.
 *
 * Upstream PSL1GHT exposes the LV2 timer API at <psl1ght/lv2/timer.h>.
 * Our SDK reorganized it to <sys/timer.h>.  This shim lets unmodified
 * PSL1GHT code build against our SDK while nudging toward the modern
 * path via the #warning below.
 *
 * Define PS3DK_SUPPRESS_PSL1GHT_COMPAT_WARN before including to silence
 * the deprecation warning (e.g. for a deliberate compat build).
 */
#ifndef __PS3DK_COMPAT_PSL1GHT_LV2_TIMER_H__
#define __PS3DK_COMPAT_PSL1GHT_LV2_TIMER_H__

#ifndef PS3DK_SUPPRESS_PSL1GHT_COMPAT_WARN
#warning "<psl1ght/lv2/timer.h> is a PSL1GHT-compat path; update the include to <sys/timer.h>"
#endif

#include <sys/timer.h>

#endif /* __PS3DK_COMPAT_PSL1GHT_LV2_TIMER_H__ */
