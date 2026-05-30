// hello-ppu-struct-byval-ret — test by-value return of 8-byte aggregate through virtual call.
#include <cstdint>
#include <cstdio>

struct Iter {
    void* sbuf;
    bool  failed;
};

struct Sink {
    virtual Iter take(Iter it, int dummy) {
        printf("[sink] sbuf=%p failed=%d dummy=%d\n", it.sbuf, (int)it.failed, dummy);
        Iter r { it.sbuf, true };
        return r;
    }
    virtual ~Sink() = default;
};

int main()
{
    int marker = 0xDEADBEEF;
    printf("[main] &marker=%p\n", (void*)&marker);
    printf("[main] sizeof(Iter)=%zu\n", sizeof(Iter));

    Iter it { &marker, false };
    Sink s;
    Sink* p = &s;
    printf("[main] calling p->take with sbuf=%p\n", it.sbuf);
    Iter result = p->take(it, 42);
    printf("[ret] sbuf=%p failed=%d\n", result.sbuf, (int)result.failed);
    printf("[main] done\n");
    return 0;
}
