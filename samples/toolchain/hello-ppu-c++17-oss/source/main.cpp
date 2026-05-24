// hello-ppu-c++17-oss — minimal isolation of the ostringstream<<int crash.
//
// Identical to hello-ppu-c++17 main + a single block that constructs a
// local std::ostringstream and inserts an int, mirroring the engine
// pattern DebugOut::msgToFile uses.

#include <array>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <stdio.h>

#include <ppu-types.h>
#include <sys/thread.h>

namespace {

inline constexpr std::string_view kBanner = "hello-ppu-c++17-oss";

}

int main()
{
    printf("%.*s\n", (int)kBanner.size(), kBanner.data());

    sys_ppu_thread_t tid;
    sysThreadGetId(&tid);
    printf("  ppu tid = 0x%016llx\n", static_cast<unsigned long long>(tid));

    printf("[oss-int] start\n");
    {
        std::ostringstream oss;
        oss << "[thread:" << 16777216 << "]\t";
        std::string s = oss.str();
        printf("[oss-int] result: %s\n", s.c_str());
    }
    printf("[oss-int] end\n");

    return 0;
}
