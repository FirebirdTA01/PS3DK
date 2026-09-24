/* Execute the production allocator with only the Lv-2 boundary replaced.
 * Suppress target constructor registration and PPC-only .fini assembly;
 * sbrk_init itself, weak symbol resolution and allocator logic stay real. */
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "heap-host.h"
#define constructor(priority) unused
#define asm(...)
#include SBRK_SOURCE
#undef asm
#undef constructor

#define MIB UINT32_C(1048576)
static unsigned char arena[256 * MIB];
static uint32_t allocation_size;
static unsigned allocation_calls, tty_calls;
static char diagnostic[512];
static jmp_buf startup_exit;
static int startup_rc;
static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "heap: FAIL line %d: %s\n", __LINE__, #x); ++failures; \
} } while (0)

int sys_memory_get_user_memory_size(sys_memory_info_t *info)
{
    info->total_user_memory = sizeof(arena);
    info->available_user_memory = AVAILABLE;
    return INFO_ERROR;
}
int sys_memory_allocate(uint32_t bytes, uint64_t flags, sys_addr_t *addr)
{
    ++allocation_calls;
    CHECK(flags == SYS_MEMORY_PAGE_SIZE_1M);
    allocation_size = bytes;
    *addr = (uintptr_t)arena;
    return ALLOC_ERROR;
}
int sys_memory_free(sys_addr_t addr) { (void)addr; return 0; }
int sysTtyWrite(int channel, const void *text, uint32_t len, uint32_t *written)
{
    CHECK(channel == 2);
    CHECK(len < sizeof(diagnostic));
    ++tty_calls;
    if (len >= sizeof(diagnostic)) len = sizeof(diagnostic) - 1;
    memcpy(diagnostic, text, len);
    diagnostic[len] = '\0';
    *written = len;
    return 0;
}
void sysProcessExit(int code)
{
    startup_rc = code;
    longjmp(startup_exit, 1);
}

int main(void)
{
    struct _reent alternate = {0};
    int terminated = setjmp(startup_exit);
    if (!terminated) sbrk_init();
    if (EXPECT_FAILURE) {
        CHECK(terminated && startup_rc == 1);
        CHECK(tty_calls == 1);
        CHECK(strstr(diagnostic, "PS3TC heap: startup failure") != NULL);
        CHECK(strstr(diagnostic, "requested=") != NULL);
        CHECK(strstr(diagnostic, "error=") != NULL);
        CHECK(allocation_calls == EXPECT_ALLOCATION_CALLS);
    } else {
        CHECK(!terminated);
        CHECK(allocation_calls == 1);
        CHECK(allocation_size == EXPECT_CAPACITY);
        CHECK(tty_calls == 0);
        CHECK(__librt_sbrk_r(&alternate, 0) == (caddr_t)arena);
        CHECK(__librt_sbrk_r(&alternate, EXPECT_CAPACITY) == (caddr_t)arena);
        CHECK(__librt_sbrk_r(&alternate, 1) == (caddr_t)-1);
        CHECK(alternate._errno == ENOMEM);
        CHECK(tty_calls == 1);
        CHECK(strstr(diagnostic, "PS3TC heap: exhausted") != NULL);
        CHECK(strstr(diagnostic, "increment=1 ") != NULL);
        char expected[96];
        snprintf(expected, sizeof(expected), "break=%u capacity=%u", (unsigned)EXPECT_CAPACITY, (unsigned)EXPECT_CAPACITY);
        CHECK(strstr(diagnostic, expected) != NULL);
        CHECK(__librt_sbrk_r(&alternate, 2) == (caddr_t)-1);
        CHECK(tty_calls == 1);
        CHECK(__librt_sbrk_r(&alternate, -(ptrdiff_t)EXPECT_CAPACITY) == (caddr_t)arena);
        CHECK(__librt_sbrk_r(&alternate, PTRDIFF_MIN) == (caddr_t)arena);
        CHECK(__librt_sbrk_r(&alternate, PTRDIFF_MAX) == (caddr_t)-1);
        CHECK(tty_calls == 1);
    }
    if (!failures) puts("librt-heap: PASS");
    return failures ? 1 : 0;
}
