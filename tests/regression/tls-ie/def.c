/* Defines the thread-local objects; accesses here are local-exec.  */
#include "tls-ie.h"

__thread int tls_ie_counter = 41;
__thread long long tls_ie_wide = 0x0123456789abcdefLL;
__thread int tls_ie_array[4] = { 2, 3, 5, 7 };

int *tls_ie_counter_le (void) { return &tls_ie_counter; }
long long *tls_ie_wide_le (void) { return &tls_ie_wide; }
int *tls_ie_array_le (void) { return &tls_ie_array[0]; }
