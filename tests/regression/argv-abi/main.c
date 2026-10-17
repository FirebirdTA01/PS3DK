/*
 * argv-abi: main receives the arguments lv2 starts the process with, in both ABIs.
 *
 * lv2 hands a process its argument vector as 64-bit effective addresses; under ILP32 the CRT narrows it to 32-bit
 * pointers before main (runtime/lv2/crt/crt1.c).  Without that step every ILP32 argv[i] reads the high word of an
 * entry (NULL for a 32-bit address).
 *
 *  1. synthetic (ILP32): __lv2_narrow_ea_vector on vectors of count 0, 1 and 3 with distinct addresses, the NULL
 *     terminator narrowed too, a guard area after the vector untouched, a NULL vector ignored;
 *  2. direct launch (as RPCS3 / the XMB start it): argc 1, argv[0] a non-empty path naming this SELF, argv[1] NULL;
 *  3. it then exitspawns itself with distinct, empty and spaced arguments; the spawned copy checks argc, the order and
 *     exact content of every argument, argv[0] the same path, and argv[argc] NULL.
 * (1) and (2) run in the parent; (3) in the spawned copy, which prints ARGV_ABI_OK.  Any mismatch: ARGV_ABI_FAIL.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#ifdef __LP64__
#define ABI "lp64"
#else
#define ABI "ilp32"
#endif

#define CHILD_FLAG "--argv-abi-child"

static int fails;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("ARGV_ABI_FAIL " ABI ": " __VA_ARGS__);   \
            printf("\n");                                    \
            fails++;                                         \
        }                                                    \
    } while (0)

#ifndef __LP64__
/* weak: a CRT without the narrowing step still links, and the row reports it as a failure */
int __lv2_narrow_ea_vector(void *vector, unsigned long count) __attribute__((weak));

static void put_be64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (56 - 8 * i));
}

static uint32_t get_be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* count entries + terminator as 64-bit EAs, followed by a 16-byte guard; narrow; check slots and guard */
static void synthetic(unsigned long count)
{
    unsigned char buf[8 * 5 + 16];
    const uint32_t ea[4] = { 0x10002340u, 0x0000ff01u, 0xd0123456u, 0x7fffffffu };
    memset(buf, 0xee, sizeof buf);
    for (unsigned long i = 0; i < count; i++) put_be64(buf + 8 * i, ea[i]);
    put_be64(buf + 8 * count, 0);
    unsigned char guard[16];
    memset(guard, 0xa5, sizeof guard);
    memcpy(buf + 8 * (count + 1), guard, sizeof guard);
    CHECK(__lv2_narrow_ea_vector(buf, count) == 0, "synthetic count %lu: narrowing refused", count);
    for (unsigned long i = 0; i < count; i++)
        CHECK(get_be32(buf + 4 * i) == ea[i], "synthetic count %lu slot %lu = 0x%08x, want 0x%08x", count, i,
              (unsigned)get_be32(buf + 4 * i), (unsigned)ea[i]);
    CHECK(get_be32(buf + 4 * count) == 0, "synthetic count %lu terminator slot = 0x%08x", count,
          (unsigned)get_be32(buf + 4 * count));
    CHECK(memcmp(buf + 8 * (count + 1), guard, sizeof guard) == 0, "synthetic count %lu wrote past the vector", count);
}

/* a count whose entry is not a zero terminator does not describe the vector: refused, nothing changed */
static void refused_untouched(void)
{
    unsigned char buf[8 * 3], before[8 * 3];
    put_be64(buf, 0x10002340u);
    put_be64(buf + 8, 0x10002350u);     /* claimed terminator, not zero */
    put_be64(buf + 16, 0);
    memcpy(before, buf, sizeof buf);
    CHECK(__lv2_narrow_ea_vector(buf, 1) == -1, "a non-zero terminator was not refused");
    CHECK(memcmp(buf, before, sizeof buf) == 0, "a refused vector was modified");
}
#endif

static const char *const spawn_args[] = { CHILD_FLAG, "alpha", "", "two words", "gamma", NULL };
static const char *const spawn_env[] = { "ARGV_ABI_ENV=from spawn", "ARGV_ABI_SECOND=two words", NULL };

/* constructor witness: the environment is published before constructors run */
static const char *ctor_env, *ctor_second;
__attribute__((constructor(101))) static void env_witness(void)
{
    ctor_env = getenv("ARGV_ABI_ENV");
    ctor_second = getenv("ARGV_ABI_SECOND");
}

static int env_count(void)
{
    int n = 0;
    extern char **environ;
    while (environ && environ[n]) n++;
    return n;
}

int main(int argc, const char *argv[])
{
    int child = argc >= 2 && argv[1] && strcmp(argv[1], CHILD_FLAG) == 0;
    if (!child) {
#ifndef __LP64__
        if (__lv2_narrow_ea_vector) {
            synthetic(0);
            synthetic(1);
            synthetic(3);
            CHECK(__lv2_narrow_ea_vector(NULL, 3) == -1, "a NULL vector was not refused");
            refused_untouched();
            printf("argv-abi " ABI ": synthetic narrowing checked\n");
        } else {
            CHECK(0, "the CRT has no __lv2_narrow_ea_vector (argv is not narrowed)");
        }
#endif
        CHECK(argc == 1, "direct launch argc = %d, want 1", argc);
        CHECK(argc >= 1 && argv[0] && argv[0][0] == '/', "direct launch argv[0] is not a path: %s",
              argc >= 1 && argv[0] ? argv[0] : "(NULL)");
        CHECK(argc >= 1 && argv[0] && strstr(argv[0], "argv-abi-" ABI) != NULL, "argv[0] does not name this SELF: %s",
              argc >= 1 && argv[0] ? argv[0] : "(NULL)");
        CHECK(argc >= 0 && argv[argc] == NULL, "direct launch argv[argc] is not NULL");
        CHECK(env_count() == 0 && ctor_env == NULL, "direct launch environment is not empty (%d entries)", env_count());
        if (fails) return 1;
        printf("ARGV_ABI_PARENT_OK " ABI ": argv[0]=%s; spawning with 5 arguments\n", argv[0]);
        fflush(stdout);
        exitspawn(argv[0], (const char **)spawn_args, (const char **)spawn_env, 0, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
        printf("ARGV_ABI_FAIL " ABI ": exitspawn returned\n");
        return 1;
    }
    const int n = (int)(sizeof spawn_args / sizeof spawn_args[0]) - 1;
    CHECK(argc == n + 1, "spawned argc = %d, want %d", argc, n + 1);
    CHECK(argv[0] && argv[0][0] == '/' && strstr(argv[0], "argv-abi-" ABI) != NULL, "spawned argv[0] = %s",
          argv[0] ? argv[0] : "(NULL)");
    for (int i = 0; i < n && i + 1 < argc; i++)
        CHECK(argv[i + 1] && strcmp(argv[i + 1], spawn_args[i]) == 0, "spawned argv[%d] = [%s], want [%s]", i + 1,
              argv[i + 1] ? argv[i + 1] : "(NULL)", spawn_args[i]);
    CHECK(argc >= 0 && argv[argc] == NULL, "spawned argv[argc] is not NULL");
    CHECK(ctor_env && strcmp(ctor_env, "from spawn") == 0, "constructor getenv(ARGV_ABI_ENV) = %s",
          ctor_env ? ctor_env : "(NULL)");
    CHECK(ctor_second && strcmp(ctor_second, "two words") == 0, "constructor getenv(ARGV_ABI_SECOND) = %s",
          ctor_second ? ctor_second : "(NULL)");
    CHECK(env_count() == 2, "spawned environ has %d entries, want 2", env_count());
    if (fails) return 1;
    printf("ARGV_ABI_OK " ABI ": %d arguments exact, argv[argc] NULL; environment published before constructors\n",
           argc);
    return 0;
}
