/* Runtime half of the ILP32 doloop regression.  With k = -1 and two
   requested digits the loop must write exactly two digits: digits() keeps
   the integer part of d as its first digit, so d = 0.5 gives "05".  With the
   faulty compiler CTR holds 0x1_00000001 and the loop writes past the
   guard bytes until it faults.  */
#include <stdio.h>
#include <string.h>

char *digits (char *s, const int *kp, int nd, double d);

static char buf[64];
static volatile int k = -1;

int
main (void)
{
  int kk = k;
  char *end;
  memset (buf, 'x', sizeof buf);
  end = digits (buf, &kk, 2, 0.5);
  int n = (int) (end - buf);
  int ok = n == 2 && memcmp (buf, "05", 2) == 0 && buf[2] == 'x'
	   && buf[sizeof buf - 1] == 'x';
  printf ("DOLOOP digits=%d [%.2s]\n", n, buf);
  printf ("DOLOOP %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
