/* Runtime half of the ILP32 virtual-base thunk regression.  Calling
   probe() through the Base* runs the virtual thunk, which must hand
   Derived::probe exactly the Derived address, upper half clear.  The faulty
   compiler hands it Derived + 2^32 (the zero-extended negative vcall offset
   carries into bit 32); value() then reads x from that address, which the
   RPCS3 interpreter and real hardware fault on.  */
#include <stdio.h>
#include <stdint.h>
#include "thunk.h"

int
main (void)
{
  Base *b = thunk_base ();
  Derived *d = thunk_derived ();
  unsigned long long want = (unsigned long long) (uintptr_t) d;
  unsigned long long got = b->probe ();
  int ok = got == want && (void *) b != (void *) d;
  printf ("THUNK_VCALL want=%016llx got=%016llx\n", want, got);
  if (ok)
    ok = b->value () == 0x1234;
  printf ("THUNK_VCALL %s\n", ok ? "THUNK_VCALL_OK" : "THUNK_VCALL_FAIL");
  return ok ? 0 : 1;
}
