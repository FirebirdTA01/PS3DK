/*! \file simdmath.h
 \brief Top-level forwarder for the libsimdmath SIMD math library.

  Reference-SDK convention is to ship simdmath at top level
  (`#include <simdmath.h>`), while PSL1GHT installs it under
  `<simdmath/simdmath.h>`.  vectormath/cpp/floatInVec.h in our tree
  uses the reference convention; this forwarder bridges to PSL1GHT's
  path so source written against either convention works.
*/

#ifndef PS3TC_SIMDMATH_FORWARD_H
#define PS3TC_SIMDMATH_FORWARD_H

/* The SPU vector math headers use uintptr_t and uint32_t in their load and
   store helpers and reach this header first; the reference's SPU vector
   math includes <stdint.h> itself.  */
#include <stdint.h>
#include <simdmath/simdmath.h>

#endif  /* PS3TC_SIMDMATH_FORWARD_H */
