// hello-ppu-c++17 — smoke test for the modernized PPU toolchain.
//
// Exercises C++17 features that would not compile under GCC 7.2.0 in -std=c++14:
//   - structured bindings
//   - if constexpr
//   - std::string_view
//   - std::optional
//   - class template argument deduction (CTAD)
//   - fold expressions
//   - inline variables
//
// Links against PSL1GHT's PPU runtime; runs on RPCS3 or a jailbroken PS3.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <stdio.h>

#include <ppu-types.h>
#include <sys/thread.h>

namespace {

// C++17 inline variable — previously required a .cpp with an explicit definition.
inline constexpr std::string_view kBanner = "hello-ppu-c++17";

// `if constexpr` test: pick formatter at compile time.
template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_integral_v<T>) {
        return "int(" + std::to_string(value) + ")";
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        return std::string("sv(\"") + std::string(value) + "\")";
    } else {
        return "other";
    }
}

// Fold expression: sum-all variadic.
template <typename... Ts>
constexpr auto sum(Ts... xs) {
    return (xs + ...);
}

// Returns (count, mean). Consumer uses structured bindings.
std::tuple<std::size_t, double> stats(const std::vector<int>& xs) {
    if (xs.empty()) return {0, 0.0};
    long long total = 0;
    for (int x : xs) total += x;
    return {xs.size(), static_cast<double>(total) / xs.size()};
}

std::optional<std::string> maybe_name(bool have_it) {
    if (!have_it) return std::nullopt;
    return std::string("cell");
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    printf("%s\n", std::string(kBanner).c_str());

    // Aggregate init + CTAD: deduce std::vector<int>.
    std::vector v = {1, 2, 3, 4, 5};
    auto [n, mean] = stats(v);
    printf("  stats: n=%zu mean=%.2f\n", n, mean);

    // std::string_view: no allocation.
    std::string_view sv = "toolchain";
    printf("  describe(42)        = %s\n", describe(42).c_str());
    printf("  describe(\"toolchain\")= %s\n", describe(sv).c_str());

    // Fold expression.
    constexpr auto fold_result = sum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    static_assert(fold_result == 55, "fold should compute 55 at compile time");
    printf("  sum(1..10) = %d\n", fold_result);

    // std::optional.
    if (auto name = maybe_name(true); name.has_value()) {
        printf("  name = %s\n", name->c_str());
    }

    // std::array with deduction guide.
    std::array<int, 4> fixed = {10, 20, 30, 40};
    printf("  array[2] = %d\n", fixed[2]);

    // Basic PPU thread identity check — confirms we have runtime glue.
    sys_ppu_thread_t tid;
    sysThreadGetId(&tid);
    printf("  ppu tid = 0x%016llx\n", static_cast<unsigned long long>(tid));

    printf("goodbye.\n");
    return 0;
}
