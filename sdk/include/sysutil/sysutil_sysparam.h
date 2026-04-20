/*
 * PS3 Custom Toolchain — <sysutil/sysutil_sysparam.h> naming shim.
 *
 * Cell-SDK samples include this under the `sysutil/` prefix; our
 * PSL1GHT install tree keeps it under `cell/`.  Forward so either
 * spelling resolves to the same declarations.
 */

#ifndef PS3TC_COMPAT_SYSUTIL_SYSUTIL_SYSPARAM_H
#define PS3TC_COMPAT_SYSUTIL_SYSUTIL_SYSPARAM_H

#include <cell/sysutil_sysparam.h>
#include <cell/cell_video_out.h>

#endif
