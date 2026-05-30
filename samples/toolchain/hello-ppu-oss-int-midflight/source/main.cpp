// hello-ppu-oss-int-midflight — snapshot oss memory before/after oss << 1.
#include <sstream>
#include <string>
#include <cstdint>
#include <cstdio>

int main()
{
    printf("[midflight] start\n");
    std::ostringstream oss;

    // Snapshot BEFORE int insertion
    size_t sz = sizeof(std::ostringstream);
    size_t nwords = (sz + 3) / 4;
    if (nwords > 50) nwords = 50;
    uint32_t before[50];
    for (size_t i = 0; i < nwords; ++i)
        before[i] = ((const uint32_t*)(const void*)&oss)[i];

    printf("[midflight] &oss=%p vptr=0x%08x\n", (void*)&oss, before[0]);
    printf("[midflight] oss+184 before << 1: 0x%08x\n", before[184/4]);

    // int insertion
    oss << 1;

    // Snapshot AFTER
    uint32_t after[50];
    for (size_t i = 0; i < nwords; ++i)
        after[i] = ((const uint32_t*)(const void*)&oss)[i];

    printf("[midflight] oss+184 after << 1: 0x%08x\n", after[184/4]);

    // Diff
    printf("[midflight] DIFF (changed offsets):\n");
    int changes = 0;
    for (size_t i = 0; i < nwords; ++i) {
        if (after[i] != before[i]) {
            printf("  oss+%3zu: 0x%08x -> 0x%08x\n", i*4, before[i], after[i]);
            ++changes;
        }
    }
    printf("[midflight] %d words changed\n", changes);

    std::string s = oss.str();
    printf("[midflight] result: %s\n", s.c_str());
    printf("[midflight] end\n");
    return 0;
}
