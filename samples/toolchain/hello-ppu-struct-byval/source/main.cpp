// hello-ppu-struct-byval — test by-value passing of 8-byte aggregate through virtual call.
#include <cstdint>
#include <cstdio>

// Variant A: packed (explicit 5+3 byte layout)
struct __attribute__((packed)) IterPacked {
    void* sbuf;
    bool  failed;
};

struct __attribute__((noinline)) SinkPacked {
    virtual void take(IterPacked it, int dummy) {
        printf("[sink-packed] sbuf=%p failed=%d dummy=%d\n", it.sbuf, (int)it.failed, dummy);
    }
    virtual ~SinkPacked() = default;
};

// Variant B: normal ABI layout (not packed)
struct IterNormal {
    void* sbuf;
    bool  failed;
};

struct __attribute__((noinline)) SinkNormal {
    virtual void take(IterNormal it, int dummy) {
        printf("[sink-normal] sbuf=%p failed=%d dummy=%d\n", it.sbuf, (int)it.failed, dummy);
    }
    virtual ~SinkNormal() = default;
};

int main()
{
    int marker = 0xDEADBEEF;
    printf("[main] &marker=%p\n", (void*)&marker);

    // Test 1: packed
    printf("[main] --- Test 1: packed ---\n");
    {
        IterPacked it { &marker, false };
        SinkPacked s;
        SinkPacked* p = &s;
        printf("[main] calling packed->take with sbuf=%p\n", it.sbuf);
        p->take(it, 42);
        printf("[main] packed->take returned\n");
    }

    // Test 2: normal struct
    printf("[main] --- Test 2: normal ---\n");
    {
        IterNormal it { &marker, false };
        SinkNormal s;
        SinkNormal* p = &s;
        printf("[main] calling normal->take with sbuf=%p\n", it.sbuf);
        p->take(it, 42);
        printf("[main] normal->take returned\n");
    }

    printf("[main] done\n");
    return 0;
}
