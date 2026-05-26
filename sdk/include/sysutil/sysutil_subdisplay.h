/* sdk/include/sysutil/sysutil_subdisplay.h
 *
 * sysutil-tree alias for cell/sysutil_subdisplay.h.  Samples that include
 * <sysutil_subdisplay.h> with -I .../include/sysutil resolve to the
 * real cellSubDisplay surface via this forwarder.
 */
#ifndef _PS3DK_SYSUTIL_SUBDISPLAY_H_
#define _PS3DK_SYSUTIL_SUBDISPLAY_H_
#include <sysutil/sysutil_common.h>
#include <sys/memory.h>
#include <cell/sysutil_subdisplay.h>
#endif
