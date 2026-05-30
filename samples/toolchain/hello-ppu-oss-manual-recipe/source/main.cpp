// hello-ppu-oss-manual-recipe — replicate _M_insert<long> with explicit vs implicit ostreambuf_iterator.
#include <sstream>
#include <locale>
#include <cstdint>
#include <cstdio>

int main()
{
    // Variant A: explicit iterator construction
    printf("[A] --- explicit iterator ---\n");
    {
        std::ostringstream oss;
        printf("[A] streambuf = %p\n", (void*)oss.rdbuf());
        const auto& facet = std::use_facet<std::num_put<char>>(oss.getloc());
        std::ostreambuf_iterator<char> iter(oss.rdbuf());
        printf("[A] sizeof(iter)=%zu\n", sizeof(iter));
        printf("[A] iter via cast = %p\n", *(void**)&iter);
        auto result = facet.put(iter, oss, oss.fill(), 1L);
        printf("[A] survived, result via cast = %p\n", *(void**)&result);
        printf("[A] oss.str() = \"%s\"\n", oss.str().c_str());
    }
    printf("[A] done\n\n");

    // Variant B: implicit conversion (exact _M_insert<long> pattern)
    printf("[B] --- implicit conversion ---\n");
    {
        std::ostringstream oss2;
        printf("[B] streambuf = %p\n", (void*)oss2.rdbuf());
        const auto& facet2 = std::use_facet<std::num_put<char>>(oss2.getloc());
        auto result2 = facet2.put(oss2, oss2, oss2.fill(), 1L);
        printf("[B] survived\n");
        printf("[B] result via cast = %p\n", *(void**)&result2);
        printf("[B] oss2.str() = \"%s\"\n", oss2.str().c_str());
    }
    printf("[B] done\n");

    return 0;
}
