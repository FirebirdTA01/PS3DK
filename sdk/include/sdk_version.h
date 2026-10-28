/*! \file sdk_version.h
 \brief Top-level forwarder to <cell/sdk_version.h>.

  Older SDK code reaches the version macros via a top-level
  <sdk_version.h> include; our generator writes them to
  <cell/sdk_version.h>.  This forwarder bridges the convention.
*/

#ifndef PS3TC_SDK_VERSION_FORWARD_H
#define PS3TC_SDK_VERSION_FORWARD_H

#include <cell/sdk_version.h>

/* The system API level this SDK implements (platform ABI 475.001).
   System headers and samples gate features on it (e.g. >= 0x180000
   selects the header-inline psglRescAdjustAspectRatio) and compare data
   files against it; PS3SDK_VERSION_* identifies this SDK's own release. */
#define CELL_SDK_VERSION 0x475001

#endif  /* PS3TC_SDK_VERSION_FORWARD_H */
