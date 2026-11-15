/* hello-ppu-gcm-label-reuse: is a completion label written late enough to reuse the buffers a frame read?
 *
 * Each trial renders quads into a local-memory target. Their vertices, indices and texture live in a
 * cellGcmMapMainMemory'd arena (CELL_GCM_LOCATION_MAIN), filled with a per-trial GOOD colour.
 * After the last draw the trial's completion label (application label 101) is written by the mode under test.
 * The PPU polls it with cellGcmIsLabelPassed and, the moment it has passed, rewrites the arena according to
 * the selected poison kind.  The target is then copied to main memory by an image transfer that a second
 * label (102, back-end) completes, and checked.
 *
 * Under LABEL_REUSE_HEAVY (A2 label probe):
 *   Poison kinds:
 *     VERTEX: rewrites only vertices to off-screen coordinates [2, 3]
 *     INDEX:  rewrites only the index buffer to point to in-bounds off-target vertices [4..7]
 *     TEXTURE: rewrites only texture to COLOR_POISON
 *     REFERENCE: no poison (unpoisoned baseline)
 *   Workload per trial:
 *     4 full-screen draws of heavy fragment program (fp_heavy_nokil, 131 instructions) to back up pipeline,
 *     followed by 32 large indexed draws (24,000 32-bit indices each) reading from main-memory arena.
 *   Modes:
 *     reference: baseline unpoisoned
 *     early:     command label before any draws (detector sensitivity control)
 *     command:   command label after last draw (front-end race control)
 *     texture:   texture label after last draw
 *     backend:   back-end label after last draw (A2 candidate)
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <sys/process.h>

#include "../../common/gcm_arena_common.h"
#include "arena_fp_fpo.h"
#include "arena_vp_vpo.h"

#ifdef LABEL_REUSE_HEAVY
#include "fp_heavy_nokil_fpo.h"
#endif

SYS_PROCESS_PARAM(1001, 0x100000);

#ifdef LABEL_REUSE_HEAVY
enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 8 << 20 };
enum { RT = 512, TEX_W = 1024, TEX_H = 1024, TRIALS = 32 };
#define LARGE_DRAWS     32u
#define INDEX_COUNT     24000u
#define ARENA_VBO       0x00000u
#define ARENA_QUAD_IBO  0x01000u
#define ARENA_IBO       0x020000u
#define ARENA_TEX       0x100000u
#define ARENA_READBACK  0x600000u
#define POLLS           4000000u /* x 5 us: watchdog per wait */
#define VARIANT         "heavy"
#else
enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 1 << 20 };
enum { RT = 256, TEX_W = 64, TEX_H = 64, DRAWS = 64, TRIALS = 32, MODES = 4 };
#define ARENA_TEX       0x10000u
#define ARENA_READBACK  0x40000u
#define POLLS           400000u
#define VARIANT         "light"
#define ARENA_VBO       0x00000u
#define ARENA_IBO       0x01000u
#endif

#define DONE_LABEL      101u     /* completion label written by mode under test */
#define READBACK_LABEL  102u     /* back-end label after readback transfer */
#define COLOR_POISON    0xffff00ffu
#define COLOR_CLEAR     0xff000000u
#define SENTINEL        0x5a5a5a5au
#define CRUMB_PATH      "/dev_hdd0/tmp/gcmlabel-crumb.txt"
#define SPIN_POLLS      100000u  /* spin before sleeping */

_Static_assert(DONE_LABEL >= CELL_GCM_LABEL_APP_FIRST && READBACK_LABEL <= CELL_GCM_LABEL_APP_LAST, "app labels");
#ifdef LABEL_REUSE_HEAVY
_Static_assert(ARENA_IBO + INDEX_COUNT * sizeof(uint32_t) <= ARENA_TEX, "IBO fits before texture");
_Static_assert(ARENA_TEX + TEX_W * TEX_H * 4 <= ARENA_READBACK, "texture fits before readback");
_Static_assert(ARENA_READBACK + RT * RT * 4 <= ARENA_BYTES, "readback fits the arena");
_Static_assert(ARENA_TEX % 128 == 0, "texture offset alignment");
_Static_assert(ARENA_IBO % 128 == 0, "ibo offset alignment");
#else
_Static_assert(ARENA_READBACK + RT * RT * 4 <= ARENA_BYTES, "readback fits the arena");
_Static_assert(ARENA_TEX % 128 == 0, "texture offset alignment");
#endif

static int wait_readback(uint32_t serial)
{
    for (uint32_t i = 0; i < POLLS; ++i) {
        if (cellGcmIsLabelPassed(READBACK_LABEL, serial))
            return 0;
        usleep(5);
    }
    return -1;
}

static void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void crumb(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    puts(line);
    FILE *f = fopen(CRUMB_PATH, "a");
    if (!f)
        return;
    fputs(line, f);
    fputc('\n', f);
    if (fflush(f) == 0)
        (void)fsync(fileno(f));
    fclose(f);
}

static uint32_t good_color(uint32_t seed)
{
    return 0xff000000u | (0x204060u + seed * 0x030507u);
}

#ifdef LABEL_REUSE_HEAVY

enum {
    POISON_NONE = 0,
    POISON_VERTEX,
    POISON_INDEX,
    POISON_TEXTURE
};

enum {
    MODE_REFERENCE = 0,
    MODE_EARLY,
    MODE_COMMAND,
    MODE_TEXTURE,
    MODE_BACKEND
};

static const char *get_mode_str(int m)
{
    switch (m) {
    case MODE_REFERENCE: return "reference";
    case MODE_EARLY:     return "early";
    case MODE_COMMAND:   return "command";
    case MODE_TEXTURE:   return "texture";
    case MODE_BACKEND:   return "backend";
    default:             return "unknown";
    }
}

static const char *get_poison_str(int p)
{
    switch (p) {
    case POISON_NONE:    return "none";
    case POISON_VERTEX:  return "vertex";
    case POISON_INDEX:   return "index";
    case POISON_TEXTURE: return "texture";
    default:             return "unknown";
    }
}

static void init_arena_vbo(arena_vertex *vbo)
{
    /* On-target quad: vertices 0..3 */
    vbo[0] = (arena_vertex){{-1.0f,  1.0f}, {0.0f, 0.0f}};
    vbo[1] = (arena_vertex){{ 1.0f,  1.0f}, {1.0f, 0.0f}};
    vbo[2] = (arena_vertex){{-1.0f, -1.0f}, {0.0f, 1.0f}};
    vbo[3] = (arena_vertex){{ 1.0f, -1.0f}, {1.0f, 1.0f}};

    /* In-bounds off-target block: vertices 4..7 (outside viewport [-1, 1]) */
    vbo[4] = (arena_vertex){{ 2.0f,  3.0f}, {0.0f, 0.0f}};
    vbo[5] = (arena_vertex){{ 3.0f,  3.0f}, {1.0f, 0.0f}};
    vbo[6] = (arena_vertex){{ 2.0f,  2.0f}, {0.0f, 1.0f}};
    vbo[7] = (arena_vertex){{ 3.0f,  2.0f}, {1.0f, 1.0f}};
}

static void init_unpoisoned_indices(uint32_t *ibo, uint32_t count)
{
    static const uint32_t quad_idx[6] = {0, 1, 2, 2, 1, 3};
    for (uint32_t i = 0; i < count; i += 6) {
        for (uint32_t j = 0; j < 6; ++j) {
            ibo[i + j] = quad_idx[j];
        }
    }
}

static void fill_poison_indices(uint32_t *ibo, uint32_t count)
{
    static const uint32_t off_idx[6] = {4, 5, 6, 6, 5, 7};
    for (uint32_t i = 0; i < count; i += 6) {
        for (uint32_t j = 0; j < 6; ++j) {
            ibo[i + j] = off_idx[j];
        }
    }
}

static void print_buffer_verdict(const char *buf_kind, unsigned cmd_corrupt, unsigned backend_corrupt, unsigned trials)
{
    const char *verdict_str;
    if (backend_corrupt > 0) {
        verdict_str = "refuted";
    } else if (cmd_corrupt >= 8) {
        verdict_str = "supported-for-this-workload";
    } else {
        verdict_str = "inconclusive";
    }
    crumb("A2P VERDICT buffer=%s %s (command=%u/%u backend=%u/%u; note: 0/%u is not a zero-risk bound)",
          buf_kind, verdict_str, cmd_corrupt, trials, backend_corrupt, trials, trials);
}

static int run_probe_trial(CellGcmContextData *ctx,
                           const arena_program *prog_main,
                           const arena_program *prog_heavy,
                           uint8_t *arena,
                           uint32_t arena_off,
                           uint32_t rt_off,
                           uint32_t rt_depth_off,
                           int mode,
                           int poison_kind,
                           unsigned trial,
                           uint32_t *p_serial,
                           unsigned *out_corrupt,
                           unsigned *out_expected,
                           unsigned *out_unrelated)
{
    arena_vertex *vbo = (arena_vertex *)(arena + ARENA_VBO);
    uint16_t *quad_ibo = (uint16_t *)(arena + ARENA_QUAD_IBO);
    uint32_t *ibo = (uint32_t *)(arena + ARENA_IBO);
    uint32_t *tex = (uint32_t *)(arena + ARENA_TEX);
    volatile uint32_t *readback = (volatile uint32_t *)(arena + ARENA_READBACK);

    init_arena_vbo(vbo);
    static const uint16_t q_idx[6] = {0, 1, 2, 2, 1, 3};
    memcpy(quad_ibo, q_idx, sizeof q_idx);
    init_unpoisoned_indices(ibo, INDEX_COUNT);

    uint32_t good = good_color(trial + ((uint32_t)mode << 8) + ((uint32_t)poison_kind << 16));
    arena_fill(tex, TEX_W * TEX_H, good);

    for (unsigned i = 0; i < RT * RT; i += 97)
        readback[i] = SENTINEL;

    /* Reset labels per trial */
    *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL) = 0u;
    *(volatile uint32_t *)cellGcmGetLabelAddress(READBACK_LABEL) = 0u;
    uint32_t serial = ++(*p_serial);

    cellGcmSetInvalidateVertexCache(ctx);
    cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);

    arena_set_target(ctx, rt_off, RT * 4, rt_depth_off, RT * 2, RT, RT);
    cellGcmSetClearColor(ctx, COLOR_CLEAR);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    if (mode == MODE_EARLY)
        cellGcmSetWriteCommandLabel(ctx, DONE_LABEL, serial);

    /* 4 full-screen draws of heavy fragment program (fp_heavy_nokil) to back up the pipeline */
    cellGcmSetVertexProgram(ctx, prog_heavy->vp, prog_heavy->vp_ucode);
    cellGcmSetVertexDataArray(ctx, (uint8_t)prog_heavy->position_index, 0, (uint8_t)sizeof(arena_vertex), 2,
                              CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_MAIN,
                              arena_off + ARENA_VBO + (uint32_t)offsetof(arena_vertex, pos));
    cellGcmSetVertexDataArray(ctx, (uint8_t)prog_heavy->texcoord_index, 0, (uint8_t)sizeof(arena_vertex), 2,
                              CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_MAIN,
                              arena_off + ARENA_VBO + (uint32_t)offsetof(arena_vertex, uv));
    cellGcmSetFragmentProgram(ctx, prog_heavy->fp, prog_heavy->fp_offset);
    cellGcmSetInvalidateVertexCache(ctx);
    for (unsigned h = 0; h < 4; ++h) {
        cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 6, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16,
                                 CELL_GCM_LOCATION_MAIN, arena_off + ARENA_QUAD_IBO);
    }

    /* Tens of large indexed draws (~24k 32-bit indices each) reading vertices and indices from mapped arena */
    arena_bind(ctx, prog_main, arena_off + ARENA_VBO, arena_off + ARENA_TEX, TEX_W, TEX_H);
    cellGcmSetInvalidateVertexCache(ctx);
    cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);

    for (unsigned k = 0; k < LARGE_DRAWS; ++k) {
        cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES, INDEX_COUNT, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32,
                                 CELL_GCM_LOCATION_MAIN, arena_off + ARENA_IBO);
    }

    if (mode == MODE_BACKEND || mode == MODE_REFERENCE)
        cellGcmSetWriteBackEndLabel(ctx, DONE_LABEL, serial);
    else if (mode == MODE_TEXTURE)
        cellGcmSetWriteTextureLabel(ctx, DONE_LABEL, serial);
    else if (mode == MODE_COMMAND)
        cellGcmSetWriteCommandLabel(ctx, DONE_LABEL, serial);

    /* Publish draw commands and DONE_LABEL; readback transfer is deferred until after poison rewrite */
    cellGcmFlush(ctx);

    /* Poll completion label */
    uint32_t polls = 0;
    while (!cellGcmIsLabelPassed(DONE_LABEL, serial)) {
        if (++polls > SPIN_POLLS + POLLS) {
            crumb("A2P_FAIL done-label wait mode=%s poison=%s trial=%u label=0x%08x serial=%u",
                  get_mode_str(mode), get_poison_str(poison_kind), trial,
                  *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL), serial);
            *out_unrelated = 1;
            *out_corrupt = 1;
            return -1;
        }
        if (polls > SPIN_POLLS)
            usleep(5);
    }
    uint32_t seen_val = *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL);

    /* Poison write: publication order is label seen -> poison written -> fence */
    if (poison_kind == POISON_VERTEX) {
        vbo[0] = (arena_vertex){{2.0f, 3.0f}, {0.0f, 0.0f}};
        vbo[1] = (arena_vertex){{3.0f, 3.0f}, {1.0f, 0.0f}};
        vbo[2] = (arena_vertex){{2.0f, 2.0f}, {0.0f, 1.0f}};
        vbo[3] = (arena_vertex){{3.0f, 2.0f}, {1.0f, 1.0f}};
    } else if (poison_kind == POISON_INDEX) {
        fill_poison_indices(ibo, INDEX_COUNT);
    } else if (poison_kind == POISON_TEXTURE) {
        arena_fill(tex, TEX_W * TEX_H, COLOR_POISON);
    }
    __asm__ __volatile__("sync" ::: "memory");

    /* Issue readback transfer and fence strictly AFTER poison write */
    cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN, arena_off + ARENA_READBACK, RT * 4, 0, 0,
                            rt_off, RT * 4, 0, 0, RT, RT, 4);
    cellGcmSetWriteBackEndLabel(ctx, READBACK_LABEL, serial);
    cellGcmFlush(ctx);

    if (wait_readback(serial)) {
        crumb("A2P_FAIL readback wait mode=%s poison=%s trial=%u",
              get_mode_str(mode), get_poison_str(poison_kind), trial);
        *out_unrelated = 1;
        *out_corrupt = 1;
        return -1;
    }

    /* Check readback pixels */
    unsigned bad = 0, saw_poison = 0, saw_clear = 0, unrelated = 0, sentinel = 0;
    for (unsigned i = 0; i < RT * RT; i += 97) {
        uint32_t v = readback[i];
        if (v == SENTINEL) {
            sentinel++;
            unrelated++;
            continue;
        }
        v &= 0x00ffffffu;
        if (v != (good & 0x00ffffffu)) {
            bad++;
            if (v == (COLOR_CLEAR & 0x00ffffffu)) {
                saw_clear++;
            } else if ((v & 0x00ff00ffu) == 0x00ff00ffu) {
                saw_poison++;
            } else {
                unrelated++;
            }
        }
    }

    unsigned expected = 0;
    if (poison_kind == POISON_TEXTURE)
        expected = saw_poison;
    else if (poison_kind == POISON_VERTEX || poison_kind == POISON_INDEX)
        expected = saw_clear;

    *out_corrupt = (bad > 0);
    *out_expected = expected;
    *out_unrelated = unrelated;

    crumb("A2P_TRIAL mode=%s poison=%s trial=%u label_seen=%u poison_order=seen->written->sync bad=%u expected=%u unrelated=%u sentinel=%u polls=%u",
          get_mode_str(mode), get_poison_str(poison_kind), trial, seen_val, bad, expected, unrelated, sentinel, polls);
    return 0;
}

static unsigned run_series(CellGcmContextData *ctx,
                           const arena_program *prog_main,
                           const arena_program *prog_heavy,
                           uint8_t *arena,
                           uint32_t arena_off,
                           uint32_t rt_off,
                           uint32_t rt_depth_off,
                           int mode,
                           int poison_kind,
                           uint32_t *p_serial)
{
    unsigned corrupt_total = 0;
    for (unsigned trial = 0; trial < TRIALS; ++trial) {
        unsigned c = 0, exp = 0, unrel = 0;
        run_probe_trial(ctx, prog_main, prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                        mode, poison_kind, trial, p_serial, &c, &exp, &unrel);
        if (c)
            corrupt_total++;
    }
    crumb("A2P mode=%s poison=%s corrupt=%u/%u",
          get_mode_str(mode), get_poison_str(poison_kind), corrupt_total, TRIALS);
    return corrupt_total;
}

#endif /* LABEL_REUSE_HEAVY */

int main(void)
{
    arena_display d;
    arena_program prog;
    sys_addr_t host = 0, arena_ea = 0;
    uint32_t arena_off = 0, rt_off = 0, rt_depth_off = 0;

#ifdef LABEL_REUSE_HEAVY
    crumb("A2P_BEGIN variant=%s target=%ux%u texture=%ux%u draws=%u indices=%u",
          VARIANT, RT, RT, TEX_W, TEX_H, LARGE_DRAWS, INDEX_COUNT);
#else
    unsigned corrupt[MODES] = {0}, poison[MODES] = {0}, clear[MODES] = {0};
    uint32_t polls_total[MODES] = {0};
    crumb("GCM_LABEL_REUSE_BEGIN variant=%s modes=%u trials=%u draws=%u target=%ux%u texture=%ux%u",
          VARIANT, MODES, TRIALS, DRAWS, RT, RT, TEX_W, TEX_H);
#endif

    if (gcm_host_buffer_alloc(HOST_BYTES, &host) || cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) ||
        arena_display_init(&d)) {
        crumb("A2P_FAIL init");
        return 1;
    }
    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    void *rt = arena_local_alloc(&d, 64, RT * RT * 4), *rtz = arena_local_alloc(&d, 64, RT * RT * 2);
    if (!rt || !rtz || cellGcmAddressToOffset(rt, &rt_off) || cellGcmAddressToOffset(rtz, &rt_depth_off) ||
        gcm_host_buffer_alloc(ARENA_BYTES, &arena_ea) ||
        cellGcmMapMainMemory((const void *)(uintptr_t)arena_ea, ARENA_BYTES, &arena_off) ||
        arena_program_init(&d, &prog, arena_vp_vpo, arena_fp_fpo)) {
        crumb("A2P_FAIL setup");
        return 1;
    }
    uint8_t *arena = (uint8_t *)(uintptr_t)arena_ea;

#ifdef LABEL_REUSE_HEAVY
    arena_program prog_heavy;
    if (arena_program_init(&d, &prog_heavy, arena_vp_vpo, fp_heavy_nokil_fpo)) {
        crumb("A2P_FAIL setup prog_heavy");
        return 1;
    }

    uint32_t serial = 0;

    /* Warm-up without poison to ensure shader compilation on emulator is complete */
    unsigned warmup = 0;
    int warm_ok = 0;
    for (unsigned w = 0; w < 30; ++w) {
        unsigned c = 0, exp = 0, unrel = 0;
        run_probe_trial(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                        MODE_REFERENCE, POISON_NONE, w, &serial, &c, &exp, &unrel);
        warmup++;
        if (!c && !unrel) {
            warm_ok = 1;
            break;
        }
    }
    if (!warm_ok) {
        crumb("A2P_FAIL warm-up: no clean trial in 30");
        return 1;
    }
    crumb("A2P_WARMUP trials=%u clean=1", warmup);

    /* Buffer kind 1: VERTEX poison */
    run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
               MODE_REFERENCE, POISON_VERTEX, &serial);
    run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
               MODE_EARLY, POISON_VERTEX, &serial);
    unsigned corrupt_cmd_v = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                                        MODE_COMMAND, POISON_VERTEX, &serial);
    unsigned corrupt_back_v = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                                         MODE_BACKEND, POISON_VERTEX, &serial);
    print_buffer_verdict("vertex", corrupt_cmd_v, corrupt_back_v, TRIALS);

    /* Buffer kind 2: INDEX poison */
    run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
               MODE_REFERENCE, POISON_INDEX, &serial);
    run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
               MODE_EARLY, POISON_INDEX, &serial);
    unsigned corrupt_cmd_i = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                                        MODE_COMMAND, POISON_INDEX, &serial);
    run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
               MODE_TEXTURE, POISON_INDEX, &serial);
    unsigned corrupt_back_i = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                                         MODE_BACKEND, POISON_INDEX, &serial);
    print_buffer_verdict("index", corrupt_cmd_i, corrupt_back_i, TRIALS);

    /* Buffer kind 3: TEXTURE poison */
    run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
               MODE_EARLY, POISON_TEXTURE, &serial);
    unsigned corrupt_cmd_t = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                                        MODE_COMMAND, POISON_TEXTURE, &serial);
    run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
               MODE_TEXTURE, POISON_TEXTURE, &serial);
    unsigned corrupt_back_t = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, rt_depth_off,
                                         MODE_BACKEND, POISON_TEXTURE, &serial);
    print_buffer_verdict("texture", corrupt_cmd_t, corrupt_back_t, TRIALS);

    /* Overall verdict rules */
    int v_supp = (corrupt_cmd_v >= 8 && corrupt_back_v == 0);
    int i_supp = (corrupt_cmd_i >= 8 && corrupt_back_i == 0);
    int refuted = (corrupt_back_v > 0 || corrupt_back_i > 0 || corrupt_back_t > 0);

    const char *overall;
    if (refuted) {
        overall = "refuted (backend corruption detected)";
    } else if (v_supp && i_supp) {
        overall = "supported-for-this-workload (command control caught for both vertex and index; note: 0/32 is not a zero-risk bound)";
    } else {
        overall = "inconclusive (command control not caught in >= 8/32 for both vertex and index; note: 0/32 is not a zero-risk bound)";
    }
    crumb("A2P VERDICT overall %s", overall);
    crumb("A2P DONE");
    return 0;

#else /* Light variant (LABEL_REUSE_HEAVY not defined) */

    arena_vertex *vbo = (arena_vertex *)(arena + ARENA_VBO);
    uint16_t *ibo = (uint16_t *)(arena + ARENA_IBO);
    uint32_t *tex = (uint32_t *)(arena + ARENA_TEX);
    volatile uint32_t *readback = (volatile uint32_t *)(arena + ARENA_READBACK);
    static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    memcpy(ibo, idx, sizeof idx);
    *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL) = 0u;
    *(volatile uint32_t *)cellGcmGetLabelAddress(READBACK_LABEL) = 0u;
    cellGcmCgUploadInternalConsts(ctx, prog.vp);

    uint32_t serial = 0;
    for (int mode = -1; mode < MODES; ++mode) {
        unsigned ntrials = mode < 0 ? 600u : TRIALS, warmup = 0;
        int warm_ok = 0;
        for (unsigned trial = 0; trial < ntrials; ++trial) {
            uint32_t good = good_color((uint32_t)(mode + 1) * TRIALS + trial % TRIALS);
            arena_quad(vbo, -1.0f, -1.0f, 1.0f, 1.0f);
            arena_fill(tex, TEX_W * TEX_H, good);
            memcpy(ibo, idx, sizeof idx);
            for (unsigned i = 0; i < RT * RT; i += 97)
                readback[i] = SENTINEL;
            ++serial;
            cellGcmSetInvalidateVertexCache(ctx);
            cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);
            arena_set_target(ctx, rt_off, RT * 4, rt_depth_off, RT * 2, RT, RT);
            cellGcmSetClearColor(ctx, COLOR_CLEAR);
            cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
            arena_bind(ctx, &prog, arena_off + ARENA_VBO, arena_off + ARENA_TEX, TEX_W, TEX_H);
            if (mode == 3)
                cellGcmSetWriteCommandLabel(ctx, DONE_LABEL, serial);
            for (unsigned k = 0; k < DRAWS; ++k)
                cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 6, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16,
                                         CELL_GCM_LOCATION_MAIN, arena_off + ARENA_IBO);
            if (mode <= 0)
                cellGcmSetWriteBackEndLabel(ctx, DONE_LABEL, serial);
            else if (mode == 1)
                cellGcmSetWriteTextureLabel(ctx, DONE_LABEL, serial);
            else if (mode == 2)
                cellGcmSetWriteCommandLabel(ctx, DONE_LABEL, serial);
            cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN, arena_off + ARENA_READBACK, RT * 4, 0, 0,
                                    rt_off, RT * 4, 0, 0, RT, RT, 4);
            cellGcmSetWriteBackEndLabel(ctx, READBACK_LABEL, serial);
            cellGcmFlush(ctx);

            uint32_t polls = 0;
            while (!cellGcmIsLabelPassed(DONE_LABEL, serial)) {
                if (++polls > SPIN_POLLS + POLLS) {
                    crumb("GCM_LABEL_REUSE_FAIL done-label wait mode=%d trial=%u label=0x%08x serial=%u",
                          mode, trial, *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL), serial);
                    return 1;
                }
                if (polls > SPIN_POLLS)
                    usleep(5);
            }
            if (mode >= 0) {
                arena_quad(vbo, 2.0f, 2.0f, 3.0f, 3.0f);
                arena_fill(tex, TEX_W * TEX_H, COLOR_POISON);
                polls_total[mode] += polls;
            }
            if (wait_readback(serial)) {
                crumb("GCM_LABEL_REUSE_FAIL readback wait mode=%d trial=%u", mode, trial);
                return 1;
            }
            unsigned bad = 0, saw_poison = 0, saw_clear = 0, sentinel = 0;
            for (unsigned i = 0; i < RT * RT; i += 97) {
                uint32_t v = readback[i];
                if (v == SENTINEL)
                    sentinel++;
                v &= 0x00ffffffu;
                if (v != (good & 0x00ffffffu)) {
                    bad++;
                    saw_poison += v != (COLOR_CLEAR & 0x00ffffffu);
                    saw_clear += v == (COLOR_CLEAR & 0x00ffffffu);
                }
            }
            if (sentinel) {
                crumb("GCM_LABEL_REUSE_FAIL readback not landed mode=%d trial=%u sentinel=%u", mode, trial, sentinel);
                return 1;
            }
            if (mode < 0) {
                warmup++;
                if (!bad) {
                    warm_ok = 1;
                    break;
                }
                continue;
            }
            if (bad) {
                corrupt[mode]++;
                poison[mode] += saw_poison != 0;
                clear[mode] += saw_clear != 0;
                crumb("GCM_LABEL_REUSE_CORRUPT mode=%d trial=%u bad_samples=%u poison=%u clear=%u polls=%u",
                      mode, trial, bad, saw_poison, saw_clear, polls);
            } else {
                crumb("GCM_LABEL_REUSE_TRIAL mode=%d trial=%u clean polls=%u", mode, trial, polls);
            }
        }
        if (mode < 0) {
            if (!warm_ok) {
                crumb("GCM_LABEL_REUSE_FAIL warm-up: no clean trial in 600");
                return 1;
            }
            crumb("GCM_LABEL_REUSE_WARMUP trials=%u (the last one clean)", warmup);
            continue;
        }
        crumb("GCM_LABEL_REUSE_MODE mode=%d trials=%u corrupt=%u late_texture=%u late_vertex=%u mean_polls=%u",
              mode, TRIALS, corrupt[mode], poison[mode], clear[mode], polls_total[mode] / TRIALS);
    }
    int ok = corrupt[0] == 0 && corrupt[1] == 0;
    crumb("%s variant=%s backend=%u/%u texture=%u/%u control_command=%u/%u control_early=%u/%u",
          ok ? "GCM_LABEL_REUSE_OK" : "GCM_LABEL_REUSE_FAIL", VARIANT, corrupt[0], TRIALS, corrupt[1], TRIALS,
          corrupt[2], TRIALS, corrupt[3], TRIALS);
    return ok ? 0 : 1;
#endif
}
