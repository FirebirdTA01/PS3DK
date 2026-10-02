/* Reduced from newlib _dtoa_r (the v0.19.0 candidate's printf("%6.2f")
   fault): the trip count is built from a negative int loaded with a
   zero-extending lwz and added at full register width.  Under ILP32 the
   compiler used to move that 32-bit count straight into the 64-bit CTR,
   bit 32 set, and the loop ran about 4G times (GCC patch 0050).  Kept in
   its own unit so the caller's constants do not fold the count.  */
char *
digits (char *s, const int *kp, int nd, double d)
{
  int ilim = *kp + nd + 1;
  int i;
  *s++ = (char) (48 + (int) d);
  if (ilim == 1)
    return s;
  for (i = 1;; i++)
    {
      int l;
      d *= 10.0;
      l = (int) d;
      d -= l;
      *s++ = (char) (48 + l);
      if (i + 1 == ilim)
	break;
    }
  return s;
}
