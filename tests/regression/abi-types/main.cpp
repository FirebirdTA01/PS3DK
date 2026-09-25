/*
 * abi-types - the reference toolchain's fundamental type choices.
 *
 * Under the ILP32 data model the reference PPU toolchain has
 *   int32_t = int, uint32_t = unsigned int (and the least32 types),
 *   wchar_t = 16-bit unsigned short, wint_t = int.
 * Those choices are ABI: they fix C++ mangled names (a function taking
 * uint32_t is ...j) and the size of wchar_t, L"" literals and any struct
 * holding one.  The LP64 data model has no reference counterpart; it keeps
 * the same 32-bit integer types and a 32-bit wchar_t.
 *
 * Most checks are static_asserts, so a toolchain with the wrong types does
 * not build this probe at all.  The run-time checks cover what only the
 * libraries can show: newlib's PRI*32 macros and wide-character functions
 * (newlib here is built without multibyte support, so conversions map one
 * byte to one wchar_t) and libstdc++'s wide strings and streams.
 *
 * Prints ABI_TYPES_OK, or ABI_TYPES_FAIL naming the first failed check.
 */

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <sstream>
#include <string>
#include <type_traits>

#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static constexpr bool lp64 = sizeof(long) == 8;

/* ---- compile time ---------------------------------------------------- */

static_assert(std::is_same<int32_t, int>::value, "int32_t must be int");
static_assert(std::is_same<uint32_t, unsigned int>::value,
              "uint32_t must be unsigned int");
static_assert(std::is_same<int_least32_t, int>::value,
              "int_least32_t must be int");
static_assert(std::is_same<uint_least32_t, unsigned int>::value,
              "uint_least32_t must be unsigned int");

static_assert(lp64 || sizeof(wchar_t) == 2, "ILP32 wchar_t must be 16-bit");
static_assert(lp64 || std::is_unsigned<wchar_t>::value,
              "ILP32 wchar_t must be unsigned");
static_assert(lp64 || std::is_same<wint_t, int>::value,
              "ILP32 wint_t must be int");
static_assert(!lp64 || sizeof(wchar_t) == 4, "LP64 keeps a 32-bit wchar_t");
static_assert(sizeof(L"ab") == 3 * sizeof(wchar_t), "L\"\" literal size");

struct wchar_layout {
    char c;
    wchar_t w;
};
static_assert(lp64 || (sizeof(wchar_layout) == 4
                       && offsetof(wchar_layout, w) == 2),
              "ILP32 struct holding wchar_t has the reference layout");

/* A uint32_t reference binds to an unsigned int (it cannot to an
 * unsigned long, which is what uint32_t used to be). */
static unsigned int u_storage = 7;
static uint32_t &u_ref = u_storage;

/* ---- run time -------------------------------------------------------- */

static int check(bool ok, const char *what)
{
    if (!ok)
        printf("ABI_TYPES_FAIL %s\n", what);
    return ok;
}

int main()
{
    char buf[64];
    wchar_t wbuf[32];
    int ok = 1;

    printf("abi-types: %s, sizeof(wchar_t)=%u\n", lp64 ? "LP64" : "ILP32",
           (unsigned)sizeof(wchar_t));

    ok &= check(u_ref == 7, "uint32_t& to unsigned int");

    /* newlib's inttypes.h follows the compiler's __INT32_TYPE__. */
    ok &= check(strcmp(PRIu32, "u") == 0 && strcmp(PRId32, "d") == 0,
                "PRIu32/PRId32 are the int forms");
    snprintf(buf, sizeof buf, "%" PRIu32 "/%" PRId32 "/%u",
             (uint32_t)4000000000u, (int32_t)-5, (uint32_t)12);
    ok &= check(strcmp(buf, "4000000000/-5/12") == 0, "printf of 32-bit types");

    /* Wide characters: unsigned, full 16-bit range under ILP32. */
    wchar_t top = (wchar_t)0xFFFF;
    ok &= check(lp64 || (unsigned long)top == 0xFFFFul, "wchar_t 0xFFFF unsigned");

    ok &= check(wcslen(L"hello") == 5, "wcslen");
    ok &= check(wcscmp(L"abc", L"abd") < 0, "wcscmp");
    int n = swprintf(wbuf, 32, L"%d-%ls", 42, L"ab");
    ok &= check(n == 5 && wcscmp(wbuf, L"42-ab") == 0, "swprintf %d/%ls");

    size_t converted = mbstowcs(wbuf, "abc", 32);
    ok &= check(converted == 3 && wbuf[0] == L'a' && wbuf[2] == L'c'
                && wbuf[3] == 0, "mbstowcs");
    converted = wcstombs(buf, L"xyz", sizeof buf);
    ok &= check(converted == 3 && strcmp(buf, "xyz") == 0, "wcstombs");

    /* libstdc++ wide strings and streams. */
    std::wstring ws(L"abc");
    ws += L"def";
    ok &= check(ws.size() == 6 && ws == L"abcdef", "std::wstring");
    std::wostringstream os;
    os << 12 << L'x' << L"yz";
    ok &= check(os.str() == L"12xyz", "std::wostringstream");

    printf(ok ? "ABI_TYPES_OK\n" : "ABI_TYPES_FAIL\n");
    return ok ? 0 : 1;
}
