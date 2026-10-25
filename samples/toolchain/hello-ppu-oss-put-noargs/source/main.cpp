// hello-ppu-oss-put-noargs — narrow the facet.put() caller-pack trigger.
// Validate const-pointer and inline-iterator calls. The historical null-reference
// probe is undefined C++ and is available only through an explicit build option.
#include <sstream>
#include <locale>
#include <iterator>
#include <cstdint>
#include <cstdio>

int main()
{
    int failures = 0;
    // Variant D: facet via const pointer (run FIRST)
    printf("[D] --- facet via const pointer ---\n");
    {
        std::ostringstream oss;
        printf("[D] streambuf = %p\n", (void*)oss.rdbuf());
        const auto& facet = std::use_facet<std::num_put<char>>(oss.getloc());
        std::ostreambuf_iterator<char> iter(oss.rdbuf());
        const auto* pfacet = &facet;
        auto result = pfacet->put(iter, oss, oss.fill(), 1L);
        printf("[D] survived, oss.str() = \"%s\"\n", oss.str().c_str());
        if (result.failed() || oss.str() != "1") ++failures;
    }
    printf("[D] done\n\n");

#ifdef PS3DK_ENABLE_UNDEFINED_NULL_PROBE
    // Diagnostic only: violates num_put's reference argument contract.
    // A crash here is not evidence of a toolchain or library defect.
    printf("[E] --- null ios_base ---\n");
    {
        std::ostringstream oss;
        printf("[E] streambuf = %p\n", (void*)oss.rdbuf());
        const auto& facet = std::use_facet<std::num_put<char>>(oss.getloc());
        std::ostreambuf_iterator<char> iter(oss.rdbuf());
        auto result = facet.put(iter, *static_cast<std::ios_base*>(nullptr), oss.fill(), 1L);
        printf("[E] unexpectedly returned; iterator failed=%d\n", result.failed());
    }
    printf("[E] done\n\n");
#else
    printf("[E] skipped undefined null-reference diagnostic (opt-in only)\n\n");
#endif

    // Variant C: inline iterator construction in arg list
    printf("[C] --- inline iter in arg list ---\n");
    {
        std::ostringstream oss;
        printf("[C] streambuf = %p\n", (void*)oss.rdbuf());
        const auto& facet = std::use_facet<std::num_put<char>>(oss.getloc());
        auto result = facet.put(std::ostreambuf_iterator<char>(oss.rdbuf()), oss, oss.fill(), 1L);
        printf("[C] survived, oss.str() = \"%s\"\n", oss.str().c_str());
        if (result.failed() || oss.str() != "1") ++failures;
    }
    printf("[C] done\n");

    printf("facet-put: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
