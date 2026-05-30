// hello-ppu-oss-str-only — isolate string-only insertion.
// Does oss << "[thread:" (no int), then oss.str().

#include <sstream>
#include <string>
#include <stdio.h>

int main()
{
    printf("[oss-str-only] start\n");
    {
        std::ostringstream oss;
        oss << "[thread:";
        std::string s = oss.str();
        printf("[oss-str-only] result: %s\n", s.c_str());
    }
    printf("[oss-str-only] end\n");
    return 0;
}
