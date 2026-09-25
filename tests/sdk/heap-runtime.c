/* Build with the candidate sbrk.o or isolated librt overlay. No RPCS3 is
 * launched by this probe's build; run only in a coordinated emulator slot.
 * HEAP_REQUEST selects the app definition (omit for the stock default).
 * Invalid configurations must exit before the HEAP_MAIN marker appears. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/process.h>
#ifdef HEAP_REQUEST
PS3TC_HEAP_SIZE(HEAP_REQUEST);
#endif
static void *blocks[256];
int main(void)
{
    const size_t chunk = 1024 * 1024;
    unsigned count = 0;
    puts("HEAP_MAIN");
    while (count < 256 && (blocks[count] = malloc(chunk)) != NULL) {
        /* Touch both ends to prove that the memory is mapped. */
        ((volatile unsigned char *)blocks[count])[0] = (unsigned char)count;
        ((volatile unsigned char *)blocks[count])[chunk - 1] = (unsigned char)count;
        ++count;
    }
    unsigned allocated = count;
    void *retry = malloc(chunk);
    int failures = allocated == 256 || retry != NULL;
    free(retry);
    while (count) free(blocks[--count]);
#ifdef HEAP_EXPECTED
    const uint64_t requested = (uint64_t)HEAP_EXPECTED;
#elif defined(HEAP_REQUEST)
    const uint64_t requested = (uint64_t)HEAP_REQUEST;
#else
    const uint64_t requested = UINT64_C(64) * 1024 * 1024;
#endif
    const uint64_t capacity = (requested + chunk - 1) & ~(uint64_t)(chunk - 1);
    /* libc startup and malloc metadata consume a small part of the arena. */
    if ((uint64_t)allocated * chunk >= capacity ||
        (uint64_t)(allocated + 2) * chunk < capacity) ++failures;
    printf("HEAP_RESULT failures=%d blocks=%u chunk=%u capacity=%llu ptr=%u\n",
           failures, allocated, (unsigned)chunk,
           (unsigned long long)capacity, (unsigned)sizeof(void *));
    return failures ? 1 : 0;
}
