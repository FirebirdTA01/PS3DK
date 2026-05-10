#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* Pack a 16-bit type tag into the high 16 bits of a pointer.
 *
 * This trick is widespread in LP64 codebases (V8, Mono, JVM, OpenSSL
 * ASN.1 stack, custom allocators) for storing object metadata adjacent
 * to a pointer without an extra field.  Requires uintptr_t == 64 bits
 * to have 16 free high bits above the 48-bit canonical address space.
 */

#define TAG_SHIFT 48
#define ADDR_MASK (((uintptr_t)-1) >> 16)

static uintptr_t pack(void *ptr, uint16_t tag) {
    return ((uintptr_t)ptr & ADDR_MASK)
         | ((uintptr_t)tag << TAG_SHIFT);
}

static void *unpack_ptr(uintptr_t tagged) {
    return (void *)(tagged & ADDR_MASK);
}

static uint16_t unpack_tag(uintptr_t tagged) {
    return (uint16_t)(tagged >> TAG_SHIFT);
}

int main(void) {
    int *original = (int *)malloc(64);

    printf("hello-ppu-mlp64-tagged-pointer\n");
#ifdef __LP64__
    printf("mode: LP64 (-mlp64)\n");
#else
    printf("mode: ILP32 hybrid (default)\n");
#endif
    printf("sizeof(uintptr_t) = %zu\n", sizeof(uintptr_t));
    printf("original ptr      = %p\n", (void *)original);

    uintptr_t packed = pack(original, 0xCAFE);
    printf("packed value      = 0x%llx\n", (unsigned long long)packed);

    void *recovered_ptr = unpack_ptr(packed);
    uint16_t recovered_tag = unpack_tag(packed);
    printf("unpacked ptr      = %p\n", recovered_ptr);
    printf("unpacked tag      = 0x%04x\n", recovered_tag);

    int ptr_ok = (recovered_ptr == original);
    int tag_ok = (recovered_tag == 0xCAFE);

    if (ptr_ok && tag_ok) {
        printf("result: PASS — tagged pointer round-trips, LP64 high-bit packing works\n");
    } else {
        printf("result: FAIL — sizeof(uintptr_t)=%zu insufficient for 16-bit tag at bit 48\n",
               sizeof(uintptr_t));
    }

    free(original);
    return (ptr_ok && tag_ok) ? 0 : 1;
}
