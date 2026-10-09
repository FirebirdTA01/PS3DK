/* Defined in a header without extern, the way much GCC 7-era PSL1GHT
   homebrew does it, and included by two source files: it links only if
   ppu_rules builds with -fcommon. */
#ifndef MKPATH_SHARED_H
#define MKPATH_SHARED_H
int mkpath_shared_counter;
int mkpath_bump(void);
#endif
