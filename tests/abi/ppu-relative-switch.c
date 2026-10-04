/* Relative jump-table regression for the ELF64/ILP32 PPU ABI.
   Keep the calls observable so this cannot become a table of return values.
   A linked runtime control must exercise every case and the default. */
volatile unsigned switch_effect;

#define ARM(N, K) \
  __attribute__((noinline, noclone)) unsigned arm##N(unsigned x) \
  { switch_effect += K; return x ^ K; }
ARM(0, 11)
ARM(1, 23)
ARM(2, 37)
ARM(3, 41)
ARM(4, 59)
ARM(5, 67)
ARM(6, 79)

__attribute__((noinline, noclone))
unsigned relative_switch(unsigned key, unsigned value)
{
  unsigned result;
  switch (key) {
  case 0: result = arm0(value); break;
  case 1: result = arm1(value); break;
  case 2: result = arm2(value); break;
  case 3: result = arm3(value); break;
  case 4: result = arm4(value); break;
  case 5: result = arm5(value); break;
  case 6: result = arm6(value); break;
  default: result = value ^ 101; break;
  }
  return result + switch_effect;
}

int main(void)
{
  static const unsigned weights[] = {11, 23, 37, 41, 59, 67, 79};
  unsigned key;
  for (key = 0; key != 9; ++key) {
    unsigned weight = key < 7 ? weights[key] : 101;
    unsigned effect = key < 7 ? weight : 0;
    switch_effect = 0;
    if (relative_switch(key, 0x1357) != ((0x1357 ^ weight) + effect)
        || switch_effect != effect)
      return (int)key + 1;
  }
  return 0;
}
