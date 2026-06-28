/*
 * PS3 Custom Toolchain — PSL1GHT-compat forwarding header.
 *
 * Upstream PSL1GHT exposes the sockets/net API at <psl1ght/lv2/net.h>.
 * Our SDK reorganized it to <net/net.h>.  This shim lets unmodified
 * PSL1GHT code build against our SDK while nudging toward the modern
 * path via the #warning below.
 *
 * Define PS3DK_SUPPRESS_PSL1GHT_COMPAT_WARN before including to silence
 * the deprecation warning.
 */
#ifndef __PS3DK_COMPAT_PSL1GHT_LV2_NET_H__
#define __PS3DK_COMPAT_PSL1GHT_LV2_NET_H__

#ifndef PS3DK_SUPPRESS_PSL1GHT_COMPAT_WARN
#warning "<psl1ght/lv2/net.h> is a PSL1GHT-compat path; update the include to <net/net.h>"
#endif

#include <net/net.h>

#endif /* __PS3DK_COMPAT_PSL1GHT_LV2_NET_H__ */
