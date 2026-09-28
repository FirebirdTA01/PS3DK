/* spurs-suite custom-module work unit (-mcustom-module): a relocatable
 * image the policy module in module_pm.c loads at an LS address of its
 * choosing and calls at offset 0.  Checks that _init relocated the image
 * (vtables, a pointer table) and ran its constructors. */
#include <stdint.h>

/* offset 0 of the image: the loader calls here */
__asm__(".section .interrupt,\"ax\",@progbits\n\tbr unit_entry\n\t.text");

extern "C" void _init(void);
extern "C" void _fini(void);

namespace {

struct Shape {
    virtual unsigned sides() const { return 0; }
};
struct Triangle : Shape { unsigned sides() const override { return 3; } };
struct Square : Shape { unsigned sides() const override { return 4; } };

unsigned g_built;
struct Global { Global() { g_built = 0x6c; } } g_global;

const Triangle t;
const Square s;
const Shape *const g_shapes[] = { &t, &s };

} // namespace

/* returns 0x226c when the image was relocated and constructed */
extern "C" unsigned unit_entry(void)
{
    _init();
    unsigned v = (g_shapes[0]->sides() * 10 + g_shapes[1]->sides()) << 8 | g_built;
    _fini();
    return v;
}
