/*! \file cell/error.h
 \brief Sony-SDK-source-compat shared error/return-code primitives.

  Tiny header used by Sony reference samples and most cell headers.
  CELL_OK is the canonical "no error" value (always 0); CELL_ERROR_CAST
  forces a constant to int when used inside #define CELL_X_ERROR_Y forms
  to keep the type stable in GCC under -Wsign-compare.
*/

#ifndef __PSL1GHT_CELL_ERROR_H__
#define __PSL1GHT_CELL_ERROR_H__

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_OK                0
#define CELL_ERROR_CAST(x)     ((int)(x))

/* An error code is 0x80000000 | facility << 16 | status. */
#define CELL_ERROR_ERROR_FLAG          0x80000000
#define CELL_ERROR_IS_FAILURE(e)       (((e) & CELL_ERROR_ERROR_FLAG) == CELL_ERROR_ERROR_FLAG)
#define CELL_ERROR_IS_SUCCESS(e)       (!((e) & CELL_ERROR_ERROR_FLAG))
#define CELL_ERROR_GET_FACILITY(e)     (((e) >> 16) & 0xFFF)
#define CELL_ERROR_MAKE_ERROR(fac, sts) CELL_ERROR_CAST(CELL_ERROR_ERROR_FLAG | ((fac) << 16) | (sts))
#define CELL_ERROR_CHECK_ERROR(e)      /* nothing */

#define CELL_ERROR_FACILITY_NULL                 0x000
#define CELL_ERROR_FACILITY_SYSTEM_SERVICE       0x001
#define CELL_ERROR_FACILITY_SYSTEM_UTILITY       0x002
#define CELL_ERROR_FACILITY_SYSTEM_MIDDLEWARE    0x003
#define CELL_ERROR_FACILITY_RESERVED0            0x004
#define CELL_ERROR_FACILITY_USB                  0x011
#define CELL_ERROR_FACILITY_HID                  0x012
#define CELL_ERROR_FACILITY_NET                  0x013
#define CELL_ERROR_FACILITY_MICCAM               0x014
#define CELL_ERROR_FACILITY_GFX                  0x021
#define CELL_ERROR_FACILITY_PSGL                 0x022
#define CELL_ERROR_FACILITY_SOUND                0x031
#define CELL_ERROR_FACILITY_SPU                  0x041
#define CELL_ERROR_FACILITY_DEBUG                0x051
#define CELL_ERROR_FACILITY_PERFORMANCE          0x052
#define CELL_ERROR_FACILITY_MEMGLUE              0x053
#define CELL_ERROR_FACILITY_FONT                 0x054
#define CELL_ERROR_FACILITY_CXML                 0x055
#define CELL_ERROR_FACILITY_CODEC                0x061
#define CELL_ERROR_FACILITY_HTTP                 0x071
#define CELL_ERROR_FACILITY_FT2D                 0x072
#define CELL_ERROR_FACILITY_VMATH                0x073
#define CELL_ERROR_FACILITY_SSL                  0x074
#define CELL_ERROR_FACILITY_IME                  0x075
#define CELL_ERROR_FACILITY_FIBER                0x076
#define CELL_ERROR_FACILITY_RUDP                 0x077
#define CELL_ERROR_FACILITY_STEREOSCOPIC_3D      0x078
#define CELL_ERROR_FACILITY_PSSG                 0x081
#define CELL_ERROR_FACILITY_FW                   0x082
#define CELL_ERROR_FACILITY_SCRIPT_DEBUGGER      0x083

#ifndef SUCCEEDED
#define SUCCEEDED              CELL_OK
#endif

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_ERROR_H__ */
