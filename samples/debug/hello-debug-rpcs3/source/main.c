/* hello-debug-rpcs3: a small program to practise GDB on in RPCS3.
 *
 * The debug-hello-debug-rpcs3 target stops at accumulate(). From there:
 *   bt                 main -> accumulate
 *   info locals        i, sum
 *   print *p           the point being accumulated
 *   disable 1          disable the current breakpoint before continuing
 * See docs/debugging-rpcs3.md for temporary breakpoints; stepping is unreliable.
 */
#include <stdio.h>
#include <stdint.h>

struct point {
    int32_t x;
    int32_t y;
    const char *name;
};

static const struct point points[3] = {
    { 1, 2, "first" },
    { 30, 40, "second" },
    { 500, 600, "third" },
};

/* noinline keeps a real frame for the breakpoint and the backtrace. */
static __attribute__((noinline)) int64_t accumulate(const struct point *p, int64_t total)
{
    int64_t sum = total;
    for (int i = 0; i < 2; ++i) {
        sum += (i == 0) ? p->x : p->y;
    }
    return sum;
}

int main(void)
{
    int64_t total = 0;
    for (unsigned n = 0; n < sizeof points / sizeof points[0]; ++n) {
        total = accumulate(&points[n], total);
        printf("hello-debug-rpcs3: after %s total=%lld\n", points[n].name, (long long)total);
    }
    printf("hello-debug-rpcs3: done total=%lld\n", (long long)total);
    return total == 1173 ? 0 : 1;
}
