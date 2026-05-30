// hello-ppu-oss-int-tracepoint — runtime tracepoint to pinpoint oss+184 corruption timing.
#include <sstream>
#include <string>
#include <cstdint>
#include <cstdio>

int main()
{
    std::ostringstream oss;

    uintptr_t oss_addr = (uintptr_t)&oss;
    char* raw = (char*)&oss;

    printf("[trace] &oss=%p\n", (void*)&oss);
    printf("[trace] oss+184 = 0x%08x (expect oss+4 = 0x%08lx)\n",
           *(uint32_t*)(raw + 184),
           (unsigned long)(oss_addr + 4));

    uint32_t vptr = *(uint32_t*)&oss;
    printf("[trace] vptr = 0x%08x\n", vptr);

    int32_t vbase = *(int32_t*)(uintptr_t)(vptr - 12);
    printf("[trace] vptr[-12] = 0x%08x\n", (uint32_t)vbase);

    uintptr_t basic_ios = oss_addr + (intptr_t)vbase;
    printf("[trace] basic_ios = 0x%08lx\n", (unsigned long)basic_ios);
    printf("[trace] basic_ios+120 = 0x%08x\n", *(uint32_t*)(basic_ios + 120));

    printf("[trace] match check: oss+184 == basic_ios+120 ? %s\n",
           *(uint32_t*)(raw + 184) == *(uint32_t*)(basic_ios + 120) ? "YES" : "NO");

    printf("[trace] about to do oss << 1\n");
    oss << 1;
    printf("[trace] survived\n");

    std::string s = oss.str();
    printf("[trace] result: %s\n", s.c_str());
    printf("[trace] end\n");
    return 0;
}
