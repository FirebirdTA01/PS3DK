/*! \file vectormath/cpp/vectormath_soa.h
 \brief Stub for the SOA (struct-of-arrays) variant of vectormath.

  PSL1GHT's vectormath ships only the AOS (array-of-structs) variant;
  SOA was a separate library on the original SDK.  This stub lets
  PPU TUs that include the header but don't actually reference any
  Vectormath::Soa types compile cleanly.

  TUs that *use* SOA types (matrices, vector3, transform3 in their
  parallel-storage layout) need the real SOA implementation, which
  is not yet ported into this tree.  Symptoms: undefined references
  to Vectormath::Soa::* in link or unknown-type errors at compile.
*/

#ifndef PS3TC_VECTORMATH_SOA_STUB_H
#define PS3TC_VECTORMATH_SOA_STUB_H

namespace Vectormath {
namespace Soa {
/* intentionally empty — see header comment */
} /* namespace Soa */
} /* namespace Vectormath */

#endif  /* PS3TC_VECTORMATH_SOA_STUB_H */
