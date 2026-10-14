/* Inputs for the ilp32-ptr-widen row, in their own unit so the compiler
   cannot see the constants where the conversions happen.  The pointers are
   only constructed and converted, never dereferenced.  */
#include <stdint.h>

static volatile uint32_t raw[] = { 0x7fffffffu, 0x80000000u, 0x80001000u, 0xffffffffu };

unsigned ptrwiden_count (void) { return sizeof raw / sizeof raw[0]; }
uint32_t ptrwiden_raw (unsigned i) { return raw[i]; }
void *ptrwiden_make (unsigned i) { return (void *) (uintptr_t) raw[i]; }
