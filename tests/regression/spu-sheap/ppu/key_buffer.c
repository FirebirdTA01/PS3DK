/* sheap-key-buffer: the key_buffer sample's flow, written independently.
 *
 * The PPU initialises a keyed 10240-byte heap and creates a 2 KB buffer
 * under key 11 through the firmware, zeroes it, and runs one SPU that
 * attaches to the same key, writes the first 512 primes into the buffer
 * and detaches.  The PPU prints the table (64 rows of 8) and checks it
 * against its own computation, the key's reference count, and that its
 * own Delete then frees the buffer.
 *
 * Prints "PRIME NUMBERS(512)", the rows, and the SUCCEEDED or FAILED line,
 * or SHEAP_KEY_BUFFER_HLE (see P0).
 */
#include "harness.h"
#include "key_buffer_bin.h"

#define KEY   11
#define COUNT 512

static uint8_t heap[10240] __attribute__((aligned(128)));

static int is_prime(uint32_t n)
{
    uint32_t d;

    if (n < 2)
        return 0;
    for (d = 2; d * d <= n; ++d)
        if (n % d == 0)
            return 0;
    return 1;
}

static uint64_t key_entry(unsigned key)
{
    uint64_t e;

    memcpy(&e, heap + 128 + 8 * key, 8);
    return e;
}

int main(void)
{
    sysSpuImage image;
    CellKeySheapBuffer buffer;
    uint64_t args[1][4];
    int32_t status[1] = { -1 };
    volatile uint32_t *table;
    uint32_t n = 1;
    unsigned i;
    int ok = 0, rc;

    rc = harness_load_sheap();
    if (rc == 0)
        rc = cellKeySheapInitialize(heap, sizeof(heap), 3);
    if (rc == 0 && !harness_firmware_wrote_header(heap)) {
        printf("SHEAP_KEY_BUFFER_HLE firmware cellSheap wrote no header (built-in HLE cellSheap in use)\n");
        return 0;
    }
    if (rc == 0)
        rc = cellKeySheapBufferNew(&buffer, heap, KEY, COUNT * 4);
    if (rc != 0) {
        printf("setup rc=0x%08x\n", (unsigned)rc);
        printf("## libsheap : sample_sheap_key_buffer_ppu FAILED ##\n");
        return 0;
    }
    table = (volatile uint32_t *)cellKeySheapBufferGetEa(&buffer);
    for (i = 0; i < COUNT; ++i)
        table[i] = 0;

    args[0][0] = harness_ea(heap);
    args[0][1] = KEY;
    args[0][2] = COUNT;
    args[0][3] = 0;
    if (sysSpuImageImport(&image, key_buffer_bin, 0) == 0
            && harness_run(&image, 1, args, status) == 0) {
        sysSpuImageClose(&image);
        ok = status[0] == 0;
    }

    printf("PRIME NUMBERS(%d)", COUNT);
    for (i = 0; i < COUNT; ++i) {
        if (i % 8 == 0)
            printf("\n");
        printf("%u, ", (unsigned)table[i]);
        do
            ++n;
        while (!is_prime(n));
        if (table[i] != n)
            ok = 0;
    }
    printf("\n");

    /* The SPU dropped its reference: only the PPU's is left. */
    if (key_entry(KEY) >> 32 != 1)
        ok = 0;
    cellKeySheapBufferDelete(&buffer);
    if (key_entry(KEY) != 0 || cellKeySheapQueryFree(heap) != 7936)
        ok = 0;
    printf("## libsheap : sample_sheap_key_buffer_ppu %s ##\n", ok ? "SUCCEEDED" : "FAILED");
    if (!ok)
        printf("(spu status %d, key entry %016llx)\n", (int)status[0], (unsigned long long)key_entry(KEY));
    return 0;
}
