/* Initial-exec accessors: the thread-locals are defined in def.c.  */
#include "tls-ie.h"

#ifndef PFX
#define PFX o2_
#endif
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2 (a, b)

extern __thread int tls_ie_counter;
extern __thread long long tls_ie_wide;
extern __thread int tls_ie_array[4];

int *CAT (PFX, counter_addr) (void) { return &tls_ie_counter; }
int CAT (PFX, counter_get) (void) { return tls_ie_counter; }
void CAT (PFX, counter_set) (int v) { tls_ie_counter = v; }
long long CAT (PFX, wide_get) (void) { return tls_ie_wide; }
int *CAT (PFX, array_addr) (void) { return &tls_ie_array[0]; }
int CAT (PFX, array_get) (int i) { return tls_ie_array[i]; }
