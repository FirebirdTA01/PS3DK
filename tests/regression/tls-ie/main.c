/* Runtime half of the initial-exec TLS regression (GCC patch 0057).
   ie.c and ie-o0.c reach def.c's thread-locals initial-exec: ld of the
   8-byte GOT TP-offset slot, then add of r13.  The SELF is linked with
   --no-tls-optimize so the slot load runs instead of being relaxed to
   local-exec; an lwz of the slot would read its high word.  Every
   initial-exec address must equal def.c's local-exec one, and reads and
   writes must reach the same objects.  */
#include <stdio.h>
#include <stdint.h>
#include "tls-ie.h"

static unsigned long
u (const void *p)
{
  return (unsigned long) (uintptr_t) p;
}

#define CHECK(p)							\
  do {									\
    int ok_ = p##counter_addr () == tls_ie_counter_le ()		\
              && p##array_addr () == tls_ie_array_le ();		\
    printf ("TLS_IE " #p " counter ie=%08lx le=%08lx\n",		\
            u (p##counter_addr ()), u (tls_ie_counter_le ()));		\
    ok_ &= p##counter_get () == *tls_ie_counter_le ();			\
    p##counter_set (p##counter_get () + 1);				\
    ok_ &= *tls_ie_counter_le () == p##counter_get ();			\
    ok_ &= p##wide_get () == 0x0123456789abcdefLL;			\
    ok_ &= p##array_get (3) == 7 && p##array_get (2) == 5;		\
    printf ("TLS_IE " #p " %s\n", ok_ ? "ok" : "bad");			\
    ok &= ok_;								\
  } while (0)

int
main (void)
{
  int ok = *tls_ie_counter_le () == 41;
  CHECK (o2_);
  CHECK (o0_);
  ok &= *tls_ie_counter_le () == 43;
  printf ("TLS_IE counter=%d\n", *tls_ie_counter_le ());
  printf ("TLS_IE %s\n", ok ? "TLS_IE_OK" : "TLS_IE_FAIL");
  return ok ? 0 : 1;
}
