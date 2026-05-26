/* sdk/include/sysutil/sysutil_userinfo.h
 *
 * sysutil-tree alias for cell/sysutil_userinfo.h.  Samples that include
 * <sysutil_userinfo.h> with -I .../include/sysutil resolve to the
 * real cellUserInfo surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_USERINFO_H_
#define _PS3DK_SYSUTIL_USERINFO_H_
#include <sysutil/sysutil_common.h>
#include <sys/memory.h>
#include <cell/sysutil_userinfo.h>
#endif
