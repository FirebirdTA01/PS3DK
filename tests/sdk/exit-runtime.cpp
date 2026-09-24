/* Build four separate SELFs with EXIT_MODE=0 (return), 1 (exit), 2 (_exit),
 * 3 (abort). Run only in a coordinated emulator slot.
 * Normal exit must print AFTER, DTOR_B, DTOR_A, BEFORE in that order and
 * flush BUFFERED_NO_NEWLINE. _exit/abort must print none of those markers.
 * Inspect relocations to identify __cxa_atexit versus legacy .dtors on the
 * actual PPU compiler; host compiler behavior is not evidence for PPU. */
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#ifndef EXIT_MODE
#define EXIT_MODE 0
#endif
static char stdout_buffer[4096];
static void before() { std::fprintf(stderr, "EXIT_BEFORE\n"); }
static void after() { std::fprintf(stderr, "EXIT_AFTER\n"); }
static void __attribute__((constructor(200))) register_before()
{
    if (std::atexit(before)) _exit(71);
}
struct Object {
    const char *name;
    explicit Object(const char *n) : name(n) {
        std::fprintf(stderr, "EXIT_CTOR_%s\n", name);
    }
    ~Object() { std::fprintf(stderr, "EXIT_DTOR_%s\n", name); }
};
static Object first("A"), second("B");
int main()
{
    if (std::atexit(after)) _exit(72);
    std::setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer));
    std::fprintf(stderr, "EXIT_MAIN mode=%d ptr=%u\n", EXIT_MODE, (unsigned)sizeof(void *));
    std::printf("BUFFERED_NO_NEWLINE");
#if EXIT_MODE == 0
    return 0;
#elif EXIT_MODE == 1
    std::exit(0);
#elif EXIT_MODE == 2
    _exit(0);
#else
    std::abort();
#endif
}
