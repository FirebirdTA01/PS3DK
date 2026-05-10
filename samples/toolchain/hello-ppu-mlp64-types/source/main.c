#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>

int main(void) {
    printf("hello-ppu-mlp64-types\n");
#ifdef __LP64__
    printf("mode: LP64 (-mlp64)\n");
#else
    printf("mode: ILP32 hybrid (default)\n");
#endif
    printf("sizeof(void *)    = %zu\n", sizeof(void *));
    printf("sizeof(long)      = %zu\n", sizeof(long));
    printf("sizeof(long long) = %zu\n", sizeof(long long));
    printf("sizeof(intmax_t)  = %zu\n", sizeof(intmax_t));
    printf("sizeof(uintptr_t) = %zu\n", sizeof(uintptr_t));
    printf("sizeof(intptr_t)  = %zu\n", sizeof(intptr_t));
    printf("sizeof(ptrdiff_t) = %zu\n", sizeof(ptrdiff_t));
    printf("sizeof(size_t)    = %zu\n", sizeof(size_t));
    printf("sizeof(time_t)    = %zu\n", sizeof(time_t));
    printf("sizeof(off_t)     = %zu\n", sizeof(off_t));
#ifdef __LP64__
    printf("__LP64__  = 1\n");
#else
    printf("__LP64__  = 0\n");
#endif
#ifdef __ILP32__
    printf("__ILP32__ = 1\n");
#else
    printf("__ILP32__ = 0\n");
#endif
    printf("result: PASS\n");
    return 0;
}
