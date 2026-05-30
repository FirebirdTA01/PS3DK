#include <stdio.h>

struct SV {
    unsigned len;
    const char* p;
};

__attribute__((noinline))
char f(SV sv) {
    return sv.p[0];
}

__attribute__((noinline))
char call(const char* p) {
    SV sv{1, p};
    return f(sv);
}

int main() {
    static const char data[] = "Hello, world!";
    unsigned char result = static_cast<unsigned char>(call(data));

    printf("[sv-min] data=%p\n", static_cast<const void*>(data));
    printf("[sv-min] result = 0x%02x (expect 0x48=H)\n", result);
    printf("[sv-min] done\n");
    return result == 0x48 ? 0 : 1;
}
