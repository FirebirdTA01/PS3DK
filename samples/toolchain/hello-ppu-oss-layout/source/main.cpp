// hello-ppu-oss-layout — memory layout probe of std::ostringstream.
// Dumps raw bytes before/after oss << "[thread:" and diffs them.

#include <sstream>
#include <string>
#include <cstdint>
#include <cstdio>

int main()
{
    printf("[layout] sizeof-scan start\n");

    printf("[layout] sizeof(ostringstream) = %zu\n", sizeof(std::ostringstream));
    printf("[layout] sizeof(stringbuf)    = %zu\n", sizeof(std::stringbuf));
    printf("[layout] sizeof(streambuf)    = %zu\n", sizeof(std::streambuf));
    printf("[layout] sizeof(ios_base)     = %zu\n", sizeof(std::ios_base));
    printf("[layout] sizeof(string)       = %zu\n", sizeof(std::string));
    printf("[layout] sizeof(size_t)       = %zu\n", sizeof(size_t));
    printf("[layout] sizeof(void*)        = %zu\n", sizeof(void*));

    std::ostringstream oss;

    printf("[layout] &oss = %p\n", (void*)&oss);

    uint32_t vptr = *(const uint32_t*)(const void*)&oss;
    printf("[layout] vptr = 0x%08x\n", vptr);

    size_t sz = sizeof(std::ostringstream);
    size_t nwords = (sz + 3) / 4;
    if (nwords > 64) nwords = 64; // safety cap

    // Store BEFORE values
    uint32_t before[64];
    for (size_t i = 0; i < nwords; ++i)
        before[i] = ((const uint32_t*)(const void*)&oss)[i];

    printf("[layout] --- BEFORE oss << \"[thread:\" ---\n");
    for (size_t i = 0; i < nwords; ++i)
        printf("  oss+%3zu: 0x%08x\n", i * 4, before[i]);

    // Do the string insertion (known safe — DS-1B proven)
    oss << "[thread:";

    // Dump AFTER
    printf("[layout] --- AFTER oss << \"[thread:\" ---\n");
    for (size_t i = 0; i < nwords; ++i) {
        uint32_t val = ((const uint32_t*)(const void*)&oss)[i];
        printf("  oss+%3zu: 0x%08x", i * 4, val);
        if (val != before[i])
            printf("  *** CHANGED (was 0x%08x)", before[i]);
        printf("\n");
    }

    // Summary: which offsets changed
    printf("[layout] --- DIFF SUMMARY (changed offsets) ---\n");
    int changes = 0;
    for (size_t i = 0; i < nwords; ++i) {
        uint32_t val = ((const uint32_t*)(const void*)&oss)[i];
        if (val != before[i]) {
            printf("  oss+%3zu: 0x%08x -> 0x%08x\n", i * 4, before[i], val);
            ++changes;
        }
    }
    printf("[layout] %d words changed out of %zu\n", changes, nwords);

    printf("[layout] done\n");
    return 0;
}
