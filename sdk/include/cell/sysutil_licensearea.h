/*! \file cell/sysutil_licensearea.h
 \brief The console's license area (libsysutil_licensearea_stub).

  cellSysutilGetLicenseArea returns the region the system software is
  licensed for, one of CELL_SYSUTIL_LICENSE_AREA_*.
*/

#ifndef PS3TC_CELL_SYSUTIL_LICENSEAREA_H
#define PS3TC_CELL_SYSUTIL_LICENSEAREA_H

#include <sysutil/sysutil_common.h>   /* CELL_SYSUTIL_LICENSE_AREA_* */

#ifdef __cplusplus
extern "C" {
#endif

int cellSysutilGetLicenseArea(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_LICENSEAREA_H */
