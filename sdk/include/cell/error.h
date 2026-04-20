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

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_ERROR_H__ */
