/* lv2error() must map every LV2 status code (0x8001xxxx) to itself, because
   on the PS3 target the E* errno names are those codes (newlib patch 0018);
   0 stays 0 and a non-LV2 value is EINVAL.  lv2errno()/lv2errno_r() must
   store the code in errno and return -1.  Includes the real librt source.  */
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include LV2ERRNO_SOURCE

int
main (void)
{
  int failures = 0;
  struct _reent r = { 0 };
  for (uint32_t c = 0x80010001u; c <= 0x8001003Du; c++)
    {
      s32 code = (s32) c;
      if (lv2error (code) != code)
        { printf ("FAIL lv2error(0x%08X) = 0x%08X\n", c, (uint32_t) lv2error (code)); failures++; }
      errno = 0;
      if (lv2errno (code) != -1 || errno != code)
        { printf ("FAIL lv2errno(0x%08X): errno 0x%08X\n", c, (uint32_t) errno); failures++; }
      r._errno = 0;
      if (lv2errno_r (&r, code) != -1 || r._errno != code)
        { printf ("FAIL lv2errno_r(0x%08X): errno 0x%08X\n", c, (uint32_t) r._errno); failures++; }
    }
  if (lv2error (0) != 0)
    { puts ("FAIL lv2error(0) != 0"); failures++; }
  if (lv2error ((s32) 0x80020001u) != EINVAL)
    { puts ("FAIL non-LV2 code is not EINVAL"); failures++; }
  if (lv2errno (5) != 5)
    { puts ("FAIL lv2errno passes non-negative values through"); failures++; }
  printf ("librt-lv2errno: %d failures\n", failures);
  return failures ? 1 : 0;
}
