/* remainderf4 - for each of four float slots, compute remainder of x/y defined as x - nearest_integer(x/y) * y.
   Copyright (C) 2006, 2007 Sony Computer Entertainment Inc.
   All rights reserved.

   Redistribution and use in source and binary forms,
   with or without modification, are permitted provided that the
   following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Sony Computer Entertainment Inc nor the names
      of its contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
 */

/* PPU (AltiVec) port.  The quotient i is found as in _fmodf4: AltiVec
   rounds a fused multiply-subtract to nearest, so the SPU version's test
   (which relies on the SPU rounding toward zero) is replaced by the sign of
   |x| - i*|y| for each candidate.  Then i+1 is taken when the remainder is
   above |y|/2, or exactly |y|/2 with i odd (ties to even). */

#ifndef ___SIMD_MATH_REMAINDERF4_H___
#define ___SIMD_MATH_REMAINDERF4_H___

#include <simdmath.h>
#include <altivec.h>

#include <simdmath/_divf4.h>
#include <simdmath/_fabsf4.h>
#include <simdmath/_copysignf4.h>

//
// This returns an accurate result when |divf4(x,y)| < 2^20 and |x| < 2^128, and otherwise returns zero.
// If x == 0, the result is 0.
// If x != 0 and y == 0, the result is undefined.
static inline vector float
_remainderf4 (vector float x, vector float y)
{
  vector float q, xabs, yabs, qabs, i0, i1, i2, r1, r2, i, rem, yabshalf;
  vector signed int qi0, qi1, qi2;
  vector unsigned int inrange, neg1, neg2, odd1, odd0, odd, up;
  const vector signed int one = __vec_splatsi4(1);

  q = _divf4( x, y );
  xabs = _fabsf4( x );
  yabs = _fabsf4( y );
  qabs = _fabsf4( q );

  inrange = (vector unsigned int)vec_cmpgt( (vector float)(__vec_splatsu4(0x49800000)), qabs );
  inrange = vec_and( inrange, (vector unsigned int)vec_cmpgt( (vector float)(__vec_splatsu4(0x7f800000)), xabs ) );

  // i is the truncated quotient, one less, or one greater
  qi1 = vec_cts( qabs, 0 );
  qi0 = vec_sub( qi1, one );
  qi2 = vec_add( qi1, one );

  odd1 = (vector unsigned int)vec_cmpeq( vec_and( qi1, one ), one );
  odd0 = vec_nor( odd1, odd1 );

  i0 = vec_ctf( qi0, 0 );
  i1 = vec_ctf( qi1, 0 );
  i2 = vec_ctf( qi2, 0 );

  // the largest i with |x| - i*|y| >= 0
  r1 = vec_nmsub( i1, yabs, xabs );
  r2 = vec_nmsub( i2, yabs, xabs );
  neg1 = (vector unsigned int)vec_cmpgt( __vec_splatsi4(0), (vector signed int)r1 );
  neg2 = (vector unsigned int)vec_cmpgt( __vec_splatsi4(0), (vector signed int)r2 );

  i = vec_sel( i1, i0, neg1 );
  i = vec_sel( i2, i, neg2 );
  odd = vec_sel( odd1, odd0, neg1 );
  odd = vec_sel( odd0, odd, neg2 );   // i2 has the parity of i0

  // nearest integer, ties to even
  rem = vec_nmsub( i, yabs, xabs );
  yabshalf = vec_madd( yabs, __vec_splatsf4(0.5f), __vec_splatsf4(-0.0f) );
  up = vec_or( (vector unsigned int)vec_cmpgt( rem, yabshalf ),
               vec_and( (vector unsigned int)vec_cmpeq( rem, yabshalf ), odd ) );

  i = vec_sel( i, vec_add( i, __vec_splatsf4(1.0f) ), up );
  i = _copysignf4( i, q );

  return vec_sel( __vec_splatsf4(0.0f), vec_nmsub( i, y, x ), inrange );
}

#endif
