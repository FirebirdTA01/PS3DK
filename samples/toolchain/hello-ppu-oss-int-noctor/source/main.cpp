// hello-ppu-oss-int-noctor — global ostringstream, construction far from use.
#include <sstream>
#include <string>
#include <cstdio>

static std::ostringstream g_oss;

int main()
{
    printf("[noctor] start\n");
    g_oss << 1;
    std::string s = g_oss.str();
    printf("[noctor] result: %s\n", s.c_str());
    printf("[noctor] end\n");
    return 0;
}
