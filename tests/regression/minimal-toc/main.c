/*
 * minimal-toc - globals, arrays, strings and function pointers reached
 * through -mminimal-toc in both ABIs.
 *
 * Under ILP32 the compiler used to die on every function that touched the
 * TOC; GCC patch 0043 loads the minimal TOC's base with lwz from a 4-byte
 * slot, as the reference compiler does.  A wrong slot or load would read
 * every value below through a wrong address.
 *
 * Prints MINIMAL_TOC_OK, or MINIMAL_TOC_FAIL naming the first failed check.
 */

#include <stdio.h>
#include <string.h>

#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

extern int other_counter;
extern const char other_name[];
extern int other_add(int x);

static int table[5] = {3, 1, 4, 1, 5};
/* volatile: at -O2 the compiler would otherwise fold these checks to
   constants and never reach the objects through the TOC.  */
static volatile double scale = 2.5;
volatile int written;
int (*adder)(int) = other_add;

static int fail(const char *what)
{
    printf("MINIMAL_TOC_FAIL %s\n", what);
    return 0;
}

static int run(void)
{
    int sum = 0;
    for (int i = 0; i < 5; ++i)
        sum += table[i];
    if (sum != 14)
        return fail("static array");
    if (scale * 4.0 != 10.0)
        return fail("static double");
    written = sum * 3;
    if (written != 42)
        return fail("global store");
    if (adder(2) != 42 || other_counter != 42)
        return fail("function pointer and other unit's global");
    if (strcmp(other_name, "minimal") != 0)
        return fail("other unit's string");
    return 1;
}

int main(void)
{
    if (run())
        printf("MINIMAL_TOC_OK\n");
    return 0;
}
