// hello-ppu-ostream-iter-byval — test ostreambuf_iterator<char> by-value through virtual call.
#include <sstream>
#include <iterator>
#include <cstdint>
#include <cstdio>

struct Sink {
    virtual std::ostreambuf_iterator<char> take(std::ostreambuf_iterator<char> it, int dummy) {
        printf("[sink] dummy=%d\n", dummy);
        // try to use the iterator — write a char
        *it = 'X';
        return it;
    }
    virtual ~Sink() = default;
};

int main()
{
    printf("[main] sizeof(ostreambuf_iterator<char>) = %zu\n",
           sizeof(std::ostreambuf_iterator<char>));

    std::stringbuf sb;
    std::ostreambuf_iterator<char> it(&sb);
    printf("[main] iter constructed from &sb=%p\n", (void*)&sb);

    Sink s;
    Sink* p = &s;
    printf("[main] calling p->take\n");
    std::ostreambuf_iterator<char> result = p->take(it, 42);
    printf("[main] p->take returned\n");

    // Check if sb got the char
    std::string str = sb.str();
    printf("[main] sb.str() = \"%s\" (len=%zu)\n", str.c_str(), str.size());

    printf("[main] done\n");
    return 0;
}
