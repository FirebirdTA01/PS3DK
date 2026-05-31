#include "ffp_emit.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUILDER_SOURCE_CAPACITY 8192u
#define BUILDER_CMD_CAPACITY 4096u

static void write_be32(FILE *out, uint32_t value)
{
    fputc((int)((value >> 24) & 0xffu), out);
    fputc((int)((value >> 16) & 0xffu), out);
    fputc((int)((value >> 8) & 0xffu), out);
    fputc((int)(value & 0xffu), out);
}

static int file_size(const char *path, uint32_t *out_size)
{
    long size;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    fclose(file);
    if (size < 0 || size > 0x7fffffffL) return 0;
    *out_size = (uint32_t)size;
    return 1;
}

static int copy_file(FILE *out, const char *path)
{
    unsigned char buffer[4096];
    size_t got;
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    while ((got = fread(buffer, 1u, sizeof(buffer), in)) != 0u) {
        if (fwrite(buffer, 1u, got, out) != got) {
            fclose(in);
            return 0;
        }
    }
    if (ferror(in)) {
        fclose(in);
        return 0;
    }
    fclose(in);
    return 1;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    if (fputs(text, file) < 0) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int run_command(const char *command)
{
    int rc = system(command);
    return rc == 0;
}

static int parse_mask_line(char *line, uint32_t *mask)
{
    char *comment = strchr(line, '#');
    char *end;
    unsigned long value;
    if (comment) *comment = '\0';
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
        line++;
    if (*line == '\0') return 0;
    errno = 0;
    value = strtoul(line, &end, 0);
    if (errno || end == line || value > 0xfffffffful) return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end != '\0') return -1;
    *mask = (uint32_t)value;
    return 1;
}

static int compile_mask(const char *compiler, const char *work_dir,
                        uint32_t mask, FILE *out)
{
    char vp_source[BUILDER_SOURCE_CAPACITY];
    char fp_source[BUILDER_SOURCE_CAPACITY];
    char vcg_path[512];
    char fcg_path[512];
    char vpo_path[512];
    char fpo_path[512];
    char command[BUILDER_CMD_CAPACITY];
    uint32_t vp_size = 0u;
    uint32_t fp_size = 0u;

    if (!psgl_ffp_state_mask_to_cg(mask, vp_source, sizeof(vp_source),
                                   fp_source, sizeof(fp_source))) {
        fprintf(stderr, "psgl_shader_builder: unsupported mask 0x%08x\n", mask);
        return 0;
    }

    snprintf(vcg_path, sizeof(vcg_path), "%s/mask_%08x.vcg", work_dir, mask);
    snprintf(fcg_path, sizeof(fcg_path), "%s/mask_%08x.fcg", work_dir, mask);
    snprintf(vpo_path, sizeof(vpo_path), "%s/mask_%08x.vpo", work_dir, mask);
    snprintf(fpo_path, sizeof(fpo_path), "%s/mask_%08x.fpo", work_dir, mask);
    if (!write_text_file(vcg_path, vp_source) ||
        !write_text_file(fcg_path, fp_source))
        return 0;

    snprintf(command, sizeof(command),
             "\"%s\" -p sce_vp_rsx --emit-container \"%s\" \"%s\"",
             compiler, vpo_path, vcg_path);
    if (!run_command(command)) return 0;
    snprintf(command, sizeof(command),
             "\"%s\" -p sce_fp_rsx --emit-container \"%s\" \"%s\"",
             compiler, fpo_path, fcg_path);
    if (!run_command(command)) return 0;

    if (!file_size(vpo_path, &vp_size) || !file_size(fpo_path, &fp_size))
        return 0;
    write_be32(out, mask);
    write_be32(out, vp_size);
    write_be32(out, fp_size);
    return copy_file(out, vpo_path) && copy_file(out, fpo_path);
}

int main(int argc, char **argv)
{
    const char *compiler;
    const char *state_path;
    const char *out_path;
    const char *work_dir = ".";
    FILE *state;
    FILE *out;
    long count_pos;
    uint32_t count = 0u;
    char line[256];

    if (argc != 4 && argc != 5) {
        fprintf(stderr,
                "usage: %s <rsx-cg-compiler> <state.txt> <shaders.bin> [work-dir]\n",
                argv[0]);
        return 2;
    }
    compiler = argv[1];
    state_path = argv[2];
    out_path = argv[3];
    if (argc == 5) work_dir = argv[4];

    state = fopen(state_path, "rb");
    if (!state) {
        perror(state_path);
        return 1;
    }
    out = fopen(out_path, "wb");
    if (!out) {
        perror(out_path);
        fclose(state);
        return 1;
    }

    fwrite("PSGLSHDR", 1u, 8u, out);
    write_be32(out, PSGL_SHADER_LIBRARY_VERSION);
    count_pos = ftell(out);
    write_be32(out, 0u);

    while (fgets(line, sizeof(line), state)) {
        uint32_t mask = 0u;
        int parsed = parse_mask_line(line, &mask);
        if (parsed < 0) {
            fprintf(stderr, "psgl_shader_builder: invalid state line\n");
            fclose(state);
            fclose(out);
            return 1;
        }
        if (parsed == 0) continue;
        if (!compile_mask(compiler, work_dir, mask, out)) {
            fclose(state);
            fclose(out);
            return 1;
        }
        count++;
    }

    if (fseek(out, count_pos, SEEK_SET) != 0) {
        fclose(state);
        fclose(out);
        return 1;
    }
    write_be32(out, count);
    fclose(state);
    if (fclose(out) != 0) return 1;
    return 0;
}
