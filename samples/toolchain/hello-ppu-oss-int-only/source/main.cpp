// hello-ppu-oss-int-only — isolate int-only insertion.
// Does oss << 16777216 (no string), then oss.str().

#include <sstream>
#include <string>
#include <stdio.h>

int main()
{
    printf("[oss-int-only] start\n");
    {
        std::ostringstream oss;
        oss << 16777216;
        std::string s = oss.str();
        printf("[oss-int-only] result: %s\n", s.c_str());
    }
    printf("[oss-int-only] end\n");
    return 0;
}
