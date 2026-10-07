/* Virtual-base thunk under test (GCC patch 0056).

   Base is a virtual base of Derived, so Derived::probe reached through a
   Base* goes through a virtual thunk: the thunk loads the vtable pointer,
   reads the vcall offset stored below the vtable entry (here negative: the
   Base subobject sits after Derived's own fields) and adds it to `this`.
   The thunk is emitted in this unit, next to Derived::probe.

   probe() returns its incoming `this` register with all 64 bits, so a caller
   can see whether the thunk left bit 32 set.  Under ILP32 a pointer lives
   zero-extended in a 64-bit GPR: a thunk that adds a zero-extended negative
   offset at full width returns this + 2^32 - k instead of this - k.  */
#include "thunk.h"

unsigned long long
Derived::probe ()
{
  unsigned long long bits;
  __asm__ volatile ("mr %0,%1" : "=r" (bits) : "r" (this));
  return bits;
}

int
Derived::value ()
{
  return x;
}

static Derived instance;

Base *
thunk_base (void)
{
  return &instance;
}

Derived *
thunk_derived (void)
{
  return &instance;
}
