/*
 * gcm-config-abi — empirical kernel-ABI probe for cellGcmGetConfiguration.
 *
 * The CellGcmConfig widener in sdk/include/cell/gcm.h:226-240 exists
 * because PSL1GHT's gcmConfiguration is a packed 24-byte struct with
 * mode(SI) pointers while our public CellGcmConfig is 32 bytes with
 * native 64-bit pointers. To retire the widener we need to know what
 * layout the LV2 kernel actually writes: 24 bytes (PSL1GHT-correct)
 * or 32 bytes (reference-correct).
 *
 * This sample allocates a 64-byte buffer pre-filled with an 0xAA
 * sentinel, calls cellGcmGetConfiguration on it, and prints all 64
 * bytes. Interpretation:
 *
 *   - Bytes 0-23 carry kernel data in both hypotheses.
 *   - Bytes 24-31: if non-zero and coherent with localSize /
 *     memoryFrequency semantics, the kernel wrote a 32-byte native
 *     struct. If still 0xAA, the kernel stopped at byte 24 and
 *     PSL1GHT's 24-byte view is correct.
 *   - Bytes 32-63 should stay 0xAA in both hypotheses (they're
 *     beyond any plausible struct size). If not, the kernel wrote
 *     even more — interesting.
 *
 * Sample strategy:
 *   1. SYS_PROCESS_PARAM — satisfy loader.
 *   2. Minimum viable cellGcmInit (host buffer via memalign).
 *   3. Call cellGcmGetConfiguration(buf).
 *   4. Dump buf.
 *   5. Exit.
 *
 * Output lives in ~/.cache/rpcs3/RPCS3.log via sys_tty_write and is
 * extracted by scripts/rpcs3-run.sh.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <sys/process.h>
#include <cell/gcm.h>

/*
 * The probe call lives in probe.c — a TU that does NOT include
 * cell/gcm.h, so the raw NID-stub cellGcmGetConfiguration is
 * reachable without the widener inline shadowing it.
 */
extern void gcm_config_abi_probe(void *buf64);

SYS_PROCESS_PARAM(1001, 0x10000);

#define CB_SIZE    (1 * 1024 * 1024)
#define HOST_SIZE  (32 * 1024 * 1024)

static void dump_line(const char *label, const uint8_t *p, int base)
{
    printf("  %s [%02d..%02d] "
           "%02x %02x %02x %02x %02x %02x %02x %02x\n",
           label, base, base + 7,
           p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}

int main(void)
{
    printf("gcm-config-abi: begin\n");

    /* Allocate host memory for cellGcmInit. */
    void *host_addr = memalign(1024 * 1024, HOST_SIZE);
    if (!host_addr) {
        printf("gcm-config-abi: FAIL: memalign host\n");
        return 1;
    }

    int rc = cellGcmInit(CB_SIZE, HOST_SIZE, host_addr);
    if (rc != 0) {
        printf("gcm-config-abi: FAIL: cellGcmInit rc=%d\n", rc);
        free(host_addr);
        return 1;
    }
    printf("gcm-config-abi: cellGcmInit OK\n");

    /* Probe buffer. 0xAA sentinel so any untouched byte stands out.
     * Size 64 covers both hypotheses plus overflow. Aligned to 16 so
     * any reasonable struct alignment is satisfied. */
    uint8_t buf[64] __attribute__((aligned(16)));
    memset(buf, 0xAA, sizeof(buf));

    /* Call the nidgen NID stub via the isolated TU in probe.c. */
    gcm_config_abi_probe((void *)buf);

    /* Dump the 64-byte probe. Interpretation guide in the header
     * comment above. */
    printf("gcm-config-abi: dump (0xAA = untouched sentinel):\n");
    for (int i = 0; i < 64; i += 8) {
        const char *lbl;
        if      (i < 24) lbl = "H24/H32 ";
        else if (i < 32) lbl = "H32?    ";
        else             lbl = "overflow";
        dump_line(lbl, &buf[i], i);
    }

    /* Interpretation summary line — makes log-grep trivial. */
    int bytes_24_31_touched = 0;
    for (int i = 24; i < 32; i++) if (buf[i] != 0xAA) { bytes_24_31_touched = 1; break; }
    int bytes_32_63_touched = 0;
    for (int i = 32; i < 64; i++) if (buf[i] != 0xAA) { bytes_32_63_touched = 1; break; }
    printf("gcm-config-abi: VERDICT: bytes_24_31_touched=%d bytes_32_63_touched=%d\n",
           bytes_24_31_touched, bytes_32_63_touched);
    if (bytes_24_31_touched && !bytes_32_63_touched) {
        printf("gcm-config-abi: VERDICT: kernel wrote 32-byte native (H2 confirmed)\n");
    } else if (!bytes_24_31_touched && !bytes_32_63_touched) {
        printf("gcm-config-abi: VERDICT: kernel wrote 24-byte packed (H1 confirmed)\n");
    } else {
        printf("gcm-config-abi: VERDICT: unexpected — neither hypothesis holds, investigate\n");
    }

    printf("gcm-config-abi: done\n");
    free(host_addr);
    return 0;
}
