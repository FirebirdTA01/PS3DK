// hello-ppu-oss-int-stack-vs-heap — stack vs heap allocation.
#include <sstream>
#include <string>
#include <cstdio>

int main()
{
    printf("[stack] start\n");
    {
        std::ostringstream oss;
        oss << 1;
        std::string s = oss.str();
        printf("[stack] result: %s\n", s.c_str());
    }
    printf("[stack] end\n");

    printf("[heap] start\n");
    {
        auto p = new std::ostringstream();
        *p << 1;
        std::string s = p->str();
        printf("[heap] result: %s\n", s.c_str());
        delete p;
    }
    printf("[heap] end\n");

    return 0;
}
