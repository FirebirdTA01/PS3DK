// hello-ppu-oss-int-sigsegv — SIGSEGV handler reads oss+184 at crash time.
#include <sstream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <unistd.h>

static uintptr_t g_oss_addr = 0;

static void sigsegv_handler(int)
{
    // At crash time, re-read oss+184
    if (g_oss_addr != 0) {
        uint32_t val = *(const uint32_t*)(g_oss_addr + 184);
        printf("[sigsegv] oss+184 = 0x%08x (expect oss+4 = 0x%08lx)\n",
               val, (unsigned long)(g_oss_addr + 4));
    } else {
        printf("[sigsegv] g_oss_addr is NULL\n");
    }
    _exit(42);
}

int main()
{
    signal(SIGSEGV, sigsegv_handler);

    std::ostringstream oss;
    g_oss_addr = (uintptr_t)&oss;

    printf("[sigsegv] &oss = %p\n", (void*)&oss);
    printf("[sigsegv] oss+184 before = 0x%08x (expect 0x%08lx)\n",
           *(uint32_t*)((char*)&oss + 184),
           (unsigned long)(g_oss_addr + 4));

    printf("[sigsegv] about to do oss << 1\n");
    oss << 1;
    printf("[sigsegv] survived (unexpected)\n");

    return 0;
}
