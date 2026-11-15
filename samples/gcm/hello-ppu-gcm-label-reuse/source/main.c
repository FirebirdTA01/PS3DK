/* hello-ppu-gcm-label-reuse: is a completion label written late enough to reuse the buffers a frame read?
 *
 * Each trial renders quads into a local-memory target. Their vertices, indices and texture live in a
 * cellGcmMapMainMemory'd arena (CELL_GCM_LOCATION_MAIN), filled with a per-trial GOOD colour.
 * After the last draw the trial's completion label (application label 101) is written by the mode under test.
 * The PPU polls it with cellGcmIsLabelPassed and, the moment it has passed, rewrites the arena according to
 * the selected poison kind. The target is then copied to main memory by an image transfer that a second
 * label (102, back-end) completes, and checked.
 *
 * Under LABEL_REUSE_HEAVY (A2 label probe):
 *   Workload and Geometry:
 *     - Target: 512x512 checked surface (rt), plus separate local-memory backlog surface (backlog_rt).
 *     - Backlog load: 4 full-screen draws of heavy fragment program (fp_heavy_nokil, 131 instructions)
 *       into backlog_rt to saturate GPU pipelines without polluting the checked surface.
 *     - Checked region: split into a 16x16 grid of 256 non-overlapping tiles over the 512x512 surface.
 *     - Baseline: checked surface is cleared to pure COLOR_CLEAR before tile draws.
 *     - Distinct vertices: 1032 vertices total:
 *         * 1024 on-target vertices across 256 tiles (4 distinct vertices per tile, indices 0..1023)
 *         * 4 backlog full-screen quad vertices (indices 1024..1027, covering [-1, 1])
 *         * 4 valid off-screen vertices for index poison redirection (indices 1028..1031, placed at [2, 3])
 *     - Index ranges: 192 32-bit indices per tile (32 quads x 6 indices). Tile t spans indices
 *       [t * 192, (t + 1) * 192 - 1]. Each tile's quad references its own distinct vertex indices (4t..4t+3).
 *     - Total indices: 49,152 32-bit indices (192 KiB in mapped main-memory arena).
 *     - Estimated runtime: ~4-5 ms per trial; ~2.5 - 5 seconds total runtime across all 448 trials in
 *       the 14 series (well bounded under 5 minutes).
 *
 *   Poison kinds:
 *     - VERTEX: rewrites on-target vertices 0..1023 to off-screen coordinates [2.0f, 3.0f].
 *     - INDEX: rewrites tile indices to valid in-bounds off-screen vertices 1028..1031.
 *     - TEXTURE: fills texture with COLOR_POISON (0xffff00ff).
 *     - REFERENCE: unpoisoned baseline (no poison written; asserts pure clear baseline and clean coverage).
 *
 *   Modes:
 *     - reference: baseline unpoisoned (gated: must be 0/32 corruption).
 *     - early: command label before any draws (gated: must catch expected corruption >= 8/32).
 *     - command: command label after last draw (front-end race control).
 *     - texture: texture label after last draw (candidate for texture reuse; extra control for index).
 *     - backend: back-end label after last draw (candidate for vertex and index reuse).
 *
 *   Verdict Precedence:
 *     invalid > refuted > supported-for-this-workload > inconclusive
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
#include "a2p_classifier.h"
#include "arena_fp_fpo.h"
#include "arena_vp_vpo.h"

#ifdef LABEL_REUSE_HEAVY
#include "fp_heavy_nokil_fpo.h"
#endif

SYS_PROCESS_PARAM(1001, 0x100000);

#define COLOR_POISON    A2P_COLOR_POISON
#define COLOR_CLEAR     A2P_COLOR_CLEAR
#define SENTINEL        A2P_SENTINEL

#define POISON_NONE     A2P_POISON_NONE
#define POISON_VERTEX   A2P_POISON_VERTEX
#define POISON_INDEX    A2P_POISON_INDEX
#define POISON_TEXTURE  A2P_POISON_TEXTURE

#ifdef LABEL_REUSE_HEAVY
enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 8 << 20 };
enum { RT = 512, TEX_W = 1024, TEX_H = 1024, TRIALS = 32 };

#define TILES_X             16u
#define TILES_Y             16u
#define TILES               (TILES_X * TILES_Y) /* 256 tiles */
#define QUADS_PER_TILE      32u
#define INDICES_PER_TILE    (QUADS_PER_TILE * 6u) /* 192 indices */
#define TOTAL_INDICES       (TILES * INDICES_PER_TILE) /* 49,152 indices */

#define ON_TARGET_VERTS     (TILES * 4u) /* 1024 vertices (indices 0..1023) */
#define BACKLOG_VERTS_BASE  ON_TARGET_VERTS /* 4 vertices (indices 1024..1027) */
#define OFF_SCREEN_BASE     (BACKLOG_VERTS_BASE + 4u) /* 4 vertices (indices 1028..1031) */
#define TOTAL_VERTS         (OFF_SCREEN_BASE + 4u) /* 1032 distinct vertices */

#define ARENA_VBO           0x00000u
#define ARENA_QUAD_IBO      0x08000u
#define ARENA_IBO           0x10000u
#define ARENA_TEX           0x100000u
#define ARENA_READBACK      0x600000u
#define POLLS               4000000u /* x 5 us: watchdog per wait */
#define VARIANT             "heavy"

_Static_assert(ARENA_VBO + TOTAL_VERTS * sizeof(arena_vertex) <= ARENA_QUAD_IBO, "VBO fits before quad IBO");
_Static_assert(ARENA_IBO + TOTAL_INDICES * sizeof(uint32_t) <= ARENA_TEX, "IBO fits before texture");
_Static_assert(ARENA_TEX + TEX_W * TEX_H * 4 <= ARENA_READBACK, "texture fits before readback");
_Static_assert(ARENA_READBACK + RT * RT * 4 <= ARENA_BYTES, "readback fits the arena");
_Static_assert(ARENA_TEX % 128 == 0, "texture offset alignment");
_Static_assert(ARENA_IBO % 128 == 0, "ibo offset alignment");
_Static_assert(ARENA_QUAD_IBO % 128 == 0, "quad ibo alignment");

#else

enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 1 << 20 };
enum { RT = 256, TEX_W = 64, TEX_H = 64, DRAWS = 64, TRIALS = 32, MODES = 4 };
#define ARENA_TEX           0x10000u
#define ARENA_READBACK      0x40000u
#define POLLS               400000u
#define VARIANT             "light"
#define ARENA_VBO           0x00000u
#define ARENA_IBO           0x01000u

_Static_assert(ARENA_READBACK + RT * RT * 4 <= ARENA_BYTES, "readback fits the arena");
_Static_assert(ARENA_TEX % 128 == 0, "texture offset alignment");

#endif

#define DONE_LABEL          101u     /* completion label written by mode under test */
#define READBACK_LABEL      102u     /* back-end label after readback transfer */
#define CRUMB_PATH          "/dev_hdd0/tmp/gcmlabel-crumb.txt"
#define SPIN_POLLS          100000u  /* spin before sleeping */

_Static_assert(DONE_LABEL >= CELL_GCM_LABEL_APP_FIRST && READBACK_LABEL <= CELL_GCM_LABEL_APP_LAST, "app labels");

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
    char line[512];
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
    /* 1. On-target tiles: 16x16 grid of 256 non-overlapping quads */
    for (unsigned ty = 0; ty < TILES_Y; ++ty) {
        float y0 = -1.0f + (float)ty * (2.0f / (float)TILES_Y);
        float y1 = y0 + (2.0f / (float)TILES_Y);
        float v0 = (float)ty / (float)TILES_Y;
        float v1 = (float)(ty + 1) / (float)TILES_Y;
        for (unsigned tx = 0; tx < TILES_X; ++tx) {
            float x0 = -1.0f + (float)tx * (2.0f / (float)TILES_X);
            float x1 = x0 + (2.0f / (float)TILES_X);
            float u0 = (float)tx / (float)TILES_X;
            float u1 = (float)(tx + 1) / (float)TILES_X;
            unsigned base = (ty * TILES_X + tx) * 4;
            vbo[base + 0] = (arena_vertex){{x0, y1}, {u0, v0}};
            vbo[base + 1] = (arena_vertex){{x1, y1}, {u1, v0}};
            vbo[base + 2] = (arena_vertex){{x0, y0}, {u0, v1}};
            vbo[base + 3] = (arena_vertex){{x1, y0}, {u1, v1}};
        }
    }

    /* 2. Full-screen quad for backlog draws (indices 1024..1027) */
    vbo[BACKLOG_VERTS_BASE + 0] = (arena_vertex){{-1.0f,  1.0f}, {0.0f, 0.0f}};
    vbo[BACKLOG_VERTS_BASE + 1] = (arena_vertex){{ 1.0f,  1.0f}, {1.0f, 0.0f}};
    vbo[BACKLOG_VERTS_BASE + 2] = (arena_vertex){{-1.0f, -1.0f}, {0.0f, 1.0f}};
    vbo[BACKLOG_VERTS_BASE + 3] = (arena_vertex){{ 1.0f, -1.0f}, {1.0f, 1.0f}};

    /* 3. Valid in-bounds off-screen vertices for index poison redirection (indices 1028..1031) */
    vbo[OFF_SCREEN_BASE + 0] = (arena_vertex){{ 2.0f,  3.0f}, {0.0f, 0.0f}};
    vbo[OFF_SCREEN_BASE + 1] = (arena_vertex){{ 3.0f,  3.0f}, {1.0f, 0.0f}};
    vbo[OFF_SCREEN_BASE + 2] = (arena_vertex){{ 2.0f,  2.0f}, {0.0f, 1.0f}};
    vbo[OFF_SCREEN_BASE + 3] = (arena_vertex){{ 3.0f,  2.0f}, {1.0f, 1.0f}};
}

static void poison_vertices(arena_vertex *vbo)
{
    for (unsigned i = 0; i < ON_TARGET_VERTS; i += 4) {
        vbo[i + 0] = (arena_vertex){{2.0f, 3.0f}, {0.0f, 0.0f}};
        vbo[i + 1] = (arena_vertex){{3.0f, 3.0f}, {1.0f, 0.0f}};
        vbo[i + 2] = (arena_vertex){{2.0f, 2.0f}, {0.0f, 1.0f}};
        vbo[i + 3] = (arena_vertex){{3.0f, 2.0f}, {1.0f, 1.0f}};
    }
}

static void init_unpoisoned_indices(uint32_t *ibo)
{
    for (unsigned t = 0; t < TILES; ++t) {
        uint32_t base_v = t * 4;
        uint32_t *tile_ibo = ibo + t * INDICES_PER_TILE;
        for (unsigned q = 0; q < QUADS_PER_TILE; ++q) {
            tile_ibo[q * 6 + 0] = base_v + 0;
            tile_ibo[q * 6 + 1] = base_v + 1;
            tile_ibo[q * 6 + 2] = base_v + 2;
            tile_ibo[q * 6 + 3] = base_v + 2;
            tile_ibo[q * 6 + 4] = base_v + 1;
            tile_ibo[q * 6 + 5] = base_v + 3;
        }
    }
}

static void fill_poison_indices(uint32_t *ibo)
{
    for (unsigned t = 0; t < TILES; ++t) {
        uint32_t *tile_ibo = ibo + t * INDICES_PER_TILE;
        for (unsigned q = 0; q < QUADS_PER_TILE; ++q) {
            tile_ibo[q * 6 + 0] = OFF_SCREEN_BASE + 0;
            tile_ibo[q * 6 + 1] = OFF_SCREEN_BASE + 1;
            tile_ibo[q * 6 + 2] = OFF_SCREEN_BASE + 2;
            tile_ibo[q * 6 + 3] = OFF_SCREEN_BASE + 2;
            tile_ibo[q * 6 + 4] = OFF_SCREEN_BASE + 1;
            tile_ibo[q * 6 + 5] = OFF_SCREEN_BASE + 3;
        }
    }
}

static int run_probe_trial(CellGcmContextData *ctx,
                           const arena_program *prog_main,
                           const arena_program *prog_heavy,
                           uint8_t *arena,
                           uint32_t arena_off,
                           uint32_t rt_off,
                           uint32_t backlog_rt_off,
                           uint32_t rt_depth_off,
                           int mode,
                           int poison_kind,
                           unsigned trial,
                           uint32_t *p_serial,
                           int *out_trial_outcome)
{
    arena_vertex *vbo = (arena_vertex *)(arena + ARENA_VBO);
    uint16_t *quad_ibo = (uint16_t *)(arena + ARENA_QUAD_IBO);
    uint32_t *ibo = (uint32_t *)(arena + ARENA_IBO);
    uint32_t *tex = (uint32_t *)(arena + ARENA_TEX);
    volatile uint32_t *readback = (volatile uint32_t *)(arena + ARENA_READBACK);

    init_arena_vbo(vbo);
    static const uint16_t q_idx[6] = {0, 1, 2, 2, 1, 3};
    memcpy(quad_ibo, q_idx, sizeof q_idx);
    init_unpoisoned_indices(ibo);

    uint32_t good = good_color(trial + ((uint32_t)mode << 8) + ((uint32_t)poison_kind << 16));
    arena_fill(tex, TEX_W * TEX_H, good);

    for (unsigned i = 0; i < RT * RT; i += 97)
        readback[i] = SENTINEL;

    /* Reset labels per trial */
    *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL) = 0u;
    *(volatile uint32_t *)cellGcmGetLabelAddress(READBACK_LABEL) = 0u;
    uint32_t serial = ++(*p_serial);

    /* 1. Backlog draws: 4 full-screen draws of heavy fragment program (fp_heavy_nokil)
     * into a SEPARATE backlog surface to saturate pipelines without touching checked target */
    arena_set_target(ctx, backlog_rt_off, RT * 4, rt_depth_off, RT * 2, RT, RT);
    cellGcmSetVertexProgram(ctx, prog_heavy->vp, prog_heavy->vp_ucode);
    cellGcmSetVertexDataArray(ctx, (uint8_t)prog_heavy->position_index, 0, (uint8_t)sizeof(arena_vertex), 2,
                              CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_MAIN,
                              arena_off + ARENA_VBO + BACKLOG_VERTS_BASE * (uint32_t)sizeof(arena_vertex) +
                              (uint32_t)offsetof(arena_vertex, pos));
    cellGcmSetVertexDataArray(ctx, (uint8_t)prog_heavy->texcoord_index, 0, (uint8_t)sizeof(arena_vertex), 2,
                              CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_MAIN,
                              arena_off + ARENA_VBO + BACKLOG_VERTS_BASE * (uint32_t)sizeof(arena_vertex) +
                              (uint32_t)offsetof(arena_vertex, uv));
    cellGcmSetFragmentProgram(ctx, prog_heavy->fp, prog_heavy->fp_offset);
    cellGcmSetInvalidateVertexCache(ctx);
    for (unsigned h = 0; h < 4; ++h) {
        cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 6, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16,
                                 CELL_GCM_LOCATION_MAIN, arena_off + ARENA_QUAD_IBO);
    }

    /* 2. Switch to checked render target; clear baseline to pure COLOR_CLEAR before tile draws */
    arena_set_target(ctx, rt_off, RT * 4, rt_depth_off, RT * 2, RT, RT);
    cellGcmSetClearColor(ctx, COLOR_CLEAR);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    /* 3. Emit early label if MODE_EARLY */
    if (mode == MODE_EARLY)
        cellGcmSetWriteCommandLabel(ctx, DONE_LABEL, serial);

    /* 4. Draw 256 non-overlapping tiles with distinct vertices and index ranges */
    arena_bind(ctx, prog_main, arena_off + ARENA_VBO, arena_off + ARENA_TEX, TEX_W, TEX_H);
    cellGcmSetInvalidateVertexCache(ctx);
    cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);

    for (unsigned t = 0; t < TILES; ++t) {
        uint32_t tile_ibo_off = arena_off + ARENA_IBO + t * INDICES_PER_TILE * (uint32_t)sizeof(uint32_t);
        cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES,
                                 INDICES_PER_TILE, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32,
                                 CELL_GCM_LOCATION_MAIN, tile_ibo_off);
    }

    /* 5. Issue completion label for mode under test */
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
            return -1;
        }
        if (polls > SPIN_POLLS)
            usleep(5);
    }
    uint32_t seen_val = *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL);

    /* Poison write: publication order is label seen -> poison written -> fence */
    if (mode != MODE_REFERENCE && poison_kind != POISON_NONE) {
        if (poison_kind == POISON_VERTEX) {
            poison_vertices(vbo);
        } else if (poison_kind == POISON_INDEX) {
            fill_poison_indices(ibo);
        } else if (poison_kind == POISON_TEXTURE) {
            arena_fill(tex, TEX_W * TEX_H, COLOR_POISON);
        }
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
        return -1;
    }

    /* Check readback pixels using classifier */
    unsigned exp_samples = 0, unrel_samples = 0;
    int is_ref = (mode == MODE_REFERENCE || poison_kind == POISON_NONE);
    int outcome = a2p_classify_trial_samples(readback, RT * RT, good, poison_kind, is_ref,
                                             &exp_samples, &unrel_samples);
    *out_trial_outcome = outcome;

    crumb("A2P_TRIAL mode=%s poison=%s trial=%u label_seen=%u poison_order=seen->written->sync expected=%u unrelated=%u polls=%u",
          get_mode_str(mode), get_poison_str(poison_kind), trial, seen_val, exp_samples, unrel_samples, polls);
    return 0;
}

static int run_series(CellGcmContextData *ctx,
                      const arena_program *prog_main,
                      const arena_program *prog_heavy,
                      uint8_t *arena,
                      uint32_t arena_off,
                      uint32_t rt_off,
                      uint32_t backlog_rt_off,
                      uint32_t rt_depth_off,
                      int mode,
                      int poison_kind,
                      uint32_t *p_serial,
                      a2p_series_counts *out_counts)
{
    out_counts->expected = 0;
    out_counts->unrelated = 0;
    out_counts->clean = 0;

    for (unsigned trial = 0; trial < TRIALS; ++trial) {
        int outcome = A2P_TRIAL_CLEAN;
        int rc = run_probe_trial(ctx, prog_main, prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                                 mode, poison_kind, trial, p_serial, &outcome);
        if (rc != 0) {
            crumb("A2P_ABORT series mode=%s poison=%s trial=%u",
                  get_mode_str(mode), get_poison_str(poison_kind), trial);
            return -1;
        }
        if (outcome == A2P_TRIAL_EXPECTED)
            out_counts->expected++;
        else if (outcome == A2P_TRIAL_UNRELATED)
            out_counts->unrelated++;
        else
            out_counts->clean++;
    }
    crumb("A2P mode=%s poison=%s expected=%u/%u unrelated=%u/%u clean=%u/%u",
          get_mode_str(mode), get_poison_str(poison_kind),
          out_counts->expected, TRIALS, out_counts->unrelated, TRIALS, out_counts->clean, TRIALS);
    return 0;
}

static void print_buffer_verdict(const char *buf_kind,
                                 a2p_series_counts ref,
                                 a2p_series_counts early,
                                 a2p_series_counts cmd,
                                 a2p_series_counts cand,
                                 const char *cand_mode_name,
                                 const a2p_series_counts *extra,
                                 const char *extra_mode_name,
                                 char *out_verdict,
                                 size_t out_verdict_len)
{
    /* Verdict order per kind: invalid > refuted > supported-for-this-workload > inconclusive */
    const char *status = NULL;
    char reason[128] = {0};

    if (ref.expected > 0 || ref.unrelated > 0) {
        status = "invalid";
        snprintf(reason, sizeof reason, "reference corrupt");
    } else if (early.unrelated > 0) {
        status = "invalid";
        snprintf(reason, sizeof reason, "unrelated failure in early");
    } else if (early.expected < 8) {
        status = "invalid";
        snprintf(reason, sizeof reason, "detector not sensitive");
    } else if (cmd.unrelated > 0) {
        status = "invalid";
        snprintf(reason, sizeof reason, "unrelated failure in command");
    } else if (cand.unrelated > 0) {
        status = "invalid";
        snprintf(reason, sizeof reason, "unrelated failure in %s", cand_mode_name);
    } else if (extra && extra->unrelated > 0) {
        status = "invalid";
        snprintf(reason, sizeof reason, "unrelated failure in %s", extra_mode_name);
    } else if (cand.expected > 0) {
        status = "refuted";
    } else if (cmd.expected >= 8) {
        status = "supported-for-this-workload";
    } else {
        status = "inconclusive";
    }

    if (strcmp(status, "invalid") == 0) {
        snprintf(out_verdict, out_verdict_len, "invalid (%s)", reason);
        crumb("A2P VERDICT buffer=%s invalid (%s)", buf_kind, reason);
    } else {
        snprintf(out_verdict, out_verdict_len, "%s", status);
        if (extra) {
            crumb("A2P VERDICT buffer=%s %s (%s=%u/%u command=%u/%u %s=%u/%u; note: 0/%u is not a zero-risk bound)",
                  buf_kind, status, cand_mode_name, cand.expected, TRIALS, cmd.expected, TRIALS,
                  extra_mode_name, extra->expected, TRIALS, TRIALS);
        } else {
            crumb("A2P VERDICT buffer=%s %s (%s=%u/%u command=%u/%u; note: 0/%u is not a zero-risk bound)",
                  buf_kind, status, cand_mode_name, cand.expected, TRIALS, cmd.expected, TRIALS, TRIALS);
        }
    }
}

#endif /* LABEL_REUSE_HEAVY */

int main(void)
{
    arena_display d;
    arena_program prog;
    sys_addr_t host = 0, arena_ea = 0;
    uint32_t arena_off = 0, rt_off = 0, rt_depth_off = 0;

#ifdef LABEL_REUSE_HEAVY
    crumb("A2P_BEGIN variant=%s target=%ux%u texture=%ux%u tiles=%u vertices=%u indices=%u",
          VARIANT, RT, RT, TEX_W, TEX_H, TILES, TOTAL_VERTS, TOTAL_INDICES);
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

#ifdef LABEL_REUSE_HEAVY
    void *backlog_rt = arena_local_alloc(&d, 64, RT * RT * 4);
    uint32_t backlog_rt_off = 0;
    if (!rt || !rtz || !backlog_rt || cellGcmAddressToOffset(rt, &rt_off) ||
        cellGcmAddressToOffset(rtz, &rt_depth_off) || cellGcmAddressToOffset(backlog_rt, &backlog_rt_off) ||
        gcm_host_buffer_alloc(ARENA_BYTES, &arena_ea) ||
        cellGcmMapMainMemory((const void *)(uintptr_t)arena_ea, ARENA_BYTES, &arena_off) ||
        arena_program_init(&d, &prog, arena_vp_vpo, arena_fp_fpo)) {
        crumb("A2P_FAIL setup");
        return 1;
    }
#else
    if (!rt || !rtz || cellGcmAddressToOffset(rt, &rt_off) || cellGcmAddressToOffset(rtz, &rt_depth_off) ||
        gcm_host_buffer_alloc(ARENA_BYTES, &arena_ea) ||
        cellGcmMapMainMemory((const void *)(uintptr_t)arena_ea, ARENA_BYTES, &arena_off) ||
        arena_program_init(&d, &prog, arena_vp_vpo, arena_fp_fpo)) {
        crumb("A2P_FAIL setup");
        return 1;
    }
#endif

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
        int outcome = A2P_TRIAL_CLEAN;
        int rc = run_probe_trial(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                                 MODE_REFERENCE, POISON_NONE, w, &serial, &outcome);
        if (rc != 0) {
            crumb("A2P_ABORT warmup trial=%u", w);
            return 1;
        }
        warmup++;
        if (outcome == A2P_TRIAL_CLEAN) {
            warm_ok = 1;
            break;
        }
    }
    if (!warm_ok) {
        crumb("A2P_ABORT warmup: no clean trial in 30");
        return 1;
    }
    crumb("A2P_WARMUP trials=%u clean=1", warmup);

    /* Buffer kind 1: VERTEX poison */
    a2p_series_counts res_ref_v, res_early_v, res_cmd_v, res_back_v;
    if (run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_REFERENCE, POISON_VERTEX, &serial, &res_ref_v) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_EARLY, POISON_VERTEX, &serial, &res_early_v) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_COMMAND, POISON_VERTEX, &serial, &res_cmd_v) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_BACKEND, POISON_VERTEX, &serial, &res_back_v)) {
        crumb("A2P_ABORT session vertex");
        return 1;
    }
    char v_verdict[128];
    print_buffer_verdict("vertex", res_ref_v, res_early_v, res_cmd_v, res_back_v, "backend",
                         NULL, NULL, v_verdict, sizeof v_verdict);

    /* Buffer kind 2: INDEX poison */
    a2p_series_counts res_ref_i, res_early_i, res_cmd_i, res_tex_i, res_back_i;
    if (run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_REFERENCE, POISON_INDEX, &serial, &res_ref_i) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_EARLY, POISON_INDEX, &serial, &res_early_i) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_COMMAND, POISON_INDEX, &serial, &res_cmd_i) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_TEXTURE, POISON_INDEX, &serial, &res_tex_i) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_BACKEND, POISON_INDEX, &serial, &res_back_i)) {
        crumb("A2P_ABORT session index");
        return 1;
    }
    char i_verdict[128];
    print_buffer_verdict("index", res_ref_i, res_early_i, res_cmd_i, res_back_i, "backend",
                         &res_tex_i, "texture", i_verdict, sizeof i_verdict);

    /* Buffer kind 3: TEXTURE poison */
    a2p_series_counts res_ref_t, res_early_t, res_cmd_t, res_tex_t, res_back_t;
    if (run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_REFERENCE, POISON_TEXTURE, &serial, &res_ref_t) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_EARLY, POISON_TEXTURE, &serial, &res_early_t) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_COMMAND, POISON_TEXTURE, &serial, &res_cmd_t) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_TEXTURE, POISON_TEXTURE, &serial, &res_tex_t) ||
        run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off,
                   MODE_BACKEND, POISON_TEXTURE, &serial, &res_back_t)) {
        crumb("A2P_ABORT session texture");
        return 1;
    }
    char t_verdict[128];
    print_buffer_verdict("texture", res_ref_t, res_early_t, res_cmd_t, res_tex_t, "texture",
                         &res_back_t, "backend", t_verdict, sizeof t_verdict);

    /* Overall verdict rules: invalid > refuted > supported-for-this-workload > inconclusive */
    char overall[512];
    if (strncmp(v_verdict, "invalid", 7) == 0) {
        snprintf(overall, sizeof overall, "invalid (vertex %s)", strchr(v_verdict, '(') ? strchr(v_verdict, '(') : v_verdict);
    } else if (strncmp(i_verdict, "invalid", 7) == 0) {
        snprintf(overall, sizeof overall, "invalid (index %s)", strchr(i_verdict, '(') ? strchr(i_verdict, '(') : i_verdict);
    } else if (strncmp(t_verdict, "invalid", 7) == 0) {
        snprintf(overall, sizeof overall, "invalid (texture %s)", strchr(t_verdict, '(') ? strchr(t_verdict, '(') : t_verdict);
    } else if (strcmp(v_verdict, "refuted") == 0 || strcmp(i_verdict, "refuted") == 0 || strcmp(t_verdict, "refuted") == 0) {
        snprintf(overall, sizeof overall, "refuted (corruption detected: vertex=%s index=%s texture=%s)",
                 v_verdict, i_verdict, t_verdict);
    } else if (strcmp(v_verdict, "supported-for-this-workload") == 0 && strcmp(i_verdict, "supported-for-this-workload") == 0) {
        if (strcmp(t_verdict, "supported-for-this-workload") == 0) {
            snprintf(overall, sizeof overall, "supported-for-this-workload (vertex, index, and texture supported; note: 0/32 is not a zero-risk bound)");
        } else {
            snprintf(overall, sizeof overall, "supported-for-this-workload (vertex and index supported; note: 0/32 is not a zero-risk bound)");
        }
    } else {
        snprintf(overall, sizeof overall, "inconclusive (command control not caught in >= 8/32; note: 0/32 is not a zero-risk bound)");
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
