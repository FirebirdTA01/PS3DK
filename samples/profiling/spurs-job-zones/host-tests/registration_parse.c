/* Native parser acceptance probe; no target/runtime linkage required.
 * cc -std=c11 -O1 -g -Wall -Wextra -Werror -I<work>/sdk/libps3dkprof/src/spu
 *   registration_parse.c <work>/sdk/libps3dkprof/src/spu/psp_elf.c -o <out>/registration-parse-probe
 * Run: registration-parse-probe RAW_ELF [REGISTRATION_ELF]
 * RAW must be rejected. REGISTRATION, when supplied, must parse five records.
 * Parent separately verifies input SHA256 pins; this harness does not hash files.
 */
#include "psp_elf.h"
#include <stdio.h>
#include <stdlib.h>

static int probe(const char *path, int accept)
{
    FILE *f = fopen(path, "rb");
    unsigned char *bytes;
    long length;
    psp_elf_view view;
    int rc;
    if (!f) { fprintf(stderr, "open failed: %s\n", path); return 1; }
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 ||
        (unsigned long)length > PSP_ELF_SIZE_MAX || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "size/seek failed: %s\n", path); fclose(f); return 1;
    }
    bytes = malloc(length ? (size_t)length : 1u);
    if (!bytes) { fclose(f); return 1; }
    if (fread(bytes, 1, (size_t)length, f) != (size_t)length) {
        fprintf(stderr, "read failed: %s\n", path); fclose(f); free(bytes); return 1;
    }
    if (fclose(f)) { free(bytes); return 1; }
    rc = psp_elf_parse(bytes, (uint64_t)length, &view);
    printf("%s bytes=%ld parse_rc=%d\n", accept ? "REGISTRATION" : "RAW", length, rc);
    if (!accept) {
        free(bytes);
        if (rc != -1) { fputs("FAIL: raw ELF unexpectedly accepted\n", stderr); return 1; }
        puts("RAW_REFUSAL_PASS");
        return 0;
    }
    if (rc != 0 || view.n != 5) {
        fprintf(stderr, "FAIL: expected parse success and five descriptors; rc=%d count=%u\n", rc, view.n);
        free(bytes); return 1;
    }
    printf("REGISTRATION desc_addr=%08x count=%u build_id=", view.desc_addr, view.n);
    for (uint32_t i = 0; i < view.build_len; ++i) printf("%02x", view.build_id[i]);
    putchar('\n');
    for (uint32_t i = 0; i < view.n; ++i) {
        psp_desc_rec rec;
        if (psp_elf_get(&view, i, &rec)) { free(bytes); return 1; }
        printf("DESC index=%u linked_ls=%08x line=%u name=%.*s file=%.*s\n",
               i, rec.ls_addr, rec.line, (int)rec.name_len, rec.name, (int)rec.file_len, rec.file);
    }
    free(bytes);
    puts("REGISTRATION_PARSE_PASS");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fputs("usage: registration-parse-probe RAW_ELF [REGISTRATION_ELF]\n", stderr);
        return 2;
    }
    if (probe(argv[1], 0)) return 1;
    if (argc == 3 && probe(argv[2], 1)) return 1;
    return 0;
}
