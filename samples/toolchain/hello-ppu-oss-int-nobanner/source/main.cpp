// hello-ppu-oss-int-nobanner — NO printf banner before ostringstream block.
#include <sstream>
#include <string>
#include <cstdio>

int main()
{
    {
        std::ostringstream oss;
        oss << 16777216;
        std::string s = oss.str();
        printf("[nobanner] result: %s\n", s.c_str());
    }
    printf("[nobanner] end\n");
    return 0;
}
