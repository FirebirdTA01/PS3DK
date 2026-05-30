// hello-ppu-oss-put-noargs — narrow the facet.put() caller-pack trigger.
// D first (const pointer) in case it survives; then E (null ios_base); then C (inline).
#include <sstream>
#include <locale>
#include <iterator>
#include <cstdint>
#include <cstdio>

int main()
{
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
    }
    printf("[D] done\n\n");

    // Variant E: null ios_base to test arg-count interaction
    printf("[E] --- null ios_base ---\n");
    {
        std::ostringstream oss;
        printf("[E] streambuf = %p\n", (void*)oss.rdbuf());
        const auto& facet = std::use_facet<std::num_put<char>>(oss.getloc());
        std::ostreambuf_iterator<char> iter(oss.rdbuf());
        printf("[E] iter via cast = %p\n", *(void**)&iter);
        auto result = facet.put(iter, *static_cast<std::ios_base*>(nullptr), oss.fill(), 1L);
        printf("[E] survived, result via cast = %p\n", *(void**)&result);
    }
    printf("[E] done\n\n");

    // Variant C: inline iterator construction in arg list
    printf("[C] --- inline iter in arg list ---\n");
    {
        std::ostringstream oss;
        printf("[C] streambuf = %p\n", (void*)oss.rdbuf());
        const auto& facet = std::use_facet<std::num_put<char>>(oss.getloc());
        auto result = facet.put(std::ostreambuf_iterator<char>(oss.rdbuf()), oss, oss.fill(), 1L);
        printf("[C] survived, oss.str() = \"%s\"\n", oss.str().c_str());
    }
    printf("[C] done\n");

    return 0;
}
