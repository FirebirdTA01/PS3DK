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
 *     - Backlog load: 32 full-screen draws of heavy fragment program (fp_heavy_nokil, 131 instructions)
 *       into backlog_rt to saturate GPU pipelines without polluting the checked surface (~100 ms RSX time per trial).
 *     - Checked region: split into a 16x16 grid of 256 non-overlapping tiles over the 512x512 surface.
 *     - Within-tile repetition eliminated: each 32x32 tile cell contains 32 distinct sub-quads (4x8 grid,
 *       8x4 pixels each), each with its own 4 distinct vertices and 6 indices, with zero overlap between
 *       sub-quads. No sub-quad shares vertices with another, preventing early draws from masking late ones.
 *     - Total sub-quads: 8,192 sub-quads across all 256 tiles.
 *     - Sampling: 1 sample pixel per sub-quad (at sub-rectangle center), yielding 8,192 total samples per trial.
 *       The exact same sample points are evaluated in reference, early, command, and candidate series.
 *     - Sentinels: all 8,192 sampled locations are explicitly initialized to SENTINEL before each trial.
 *     - Baseline: checked surface receives a commanded pre-draw clear to pure COLOR_CLEAR before tile draws;
 *       this is a commanded baseline, not a verified observation (no drain or readback is issued inside the
 *       race window to verify it). The unpoisoned reference must show expected GOOD tile output in every tile;
 *       an all-CLEAR completed reference must FAIL (invalid), never qualifying the detector.
 *     - Distinct vertices: 32,776 vertices total:
 *         * 32,768 on-target vertices (256 tiles x 32 sub-quads x 4 vertices, indices 0..32767)
 *         * 4 backlog full-screen quad vertices (indices 32768..32771, covering [-1, 1])
 *         * 4 valid off-screen vertices for index poison redirection (indices 32772..32775, placed at [2, 3])
 *     - Index ranges: 192 32-bit indices per tile (32 sub-quads x 6 indices). Tile t spans indices
 *       [t * 192, (t + 1) * 192 - 1]. Sub-quad q of tile t references its own vertex range (4*(32t+q)..4*(32t+q)+3).
 *     - Total indices: 49,152 32-bit indices (192 KiB in mapped main-memory arena).
 *     - Monotonic session deadline: 180 s wall-clock budget (A2P_SESSION_LIMIT_US = 180,000,000 us) starting at
 *       the recorded A2P_BEGIN point, encompassing setup, warm-up, and all trial series. The deadline bound is
 *       cooperative: a blocking GCM callback, flush, or file I/O can delay the next check. Expiry is terminal:
 *       halts immediately with exit code 3 without subsequent draws, label resets, buffer rewrites, or readbacks.
 *     - Estimated runtime: ~100 ms whole-session average per trial (~45 s across all 448 trials in the 14 series
 *       plus warm-up; cooperative monotonic wall-clock bound 180 s).
 *     - Measured window observation: per-trial timestamp reports at command and back-end label points
 *       (indices 512 and 513) and PPU timebase when command label was seen. A measured command-vs-back-end gap
 *       with no corruption is an observation, not proof that all fetches finished early (observation latency,
 *       cache or reuse effects and workload sensitivity remain alternatives); print the gap as measured and
 *       draw no conclusion from it in the output.
 *     - Observation wording rule: 'command control not caught' is the observation; never write that the workload
 *       'did not let the command label pass while fetches were pending'. A session elapsed time divided by the
 *       trial count is a whole-session average, not GPU time per trial.
 *     - Emulation note: RPCS3 tracks texture rewrites through its own page protection rather than the invalidate,
 *       and r2 also fails warm-up in RPCS3, so RPCS3 cannot validate this probe family and the stale-texture
 *       reference failure under emulation is most likely the emulator (inferred; r2 drew clean references on silicon).
 *
 *   Poison kinds:
 *     - VERTEX: rewrites on-target vertices 0..32767 to off-screen coordinates [2.0f, 3.0f].
 *     - INDEX: rewrites all tile sub-quad indices to valid in-bounds off-screen vertices 32772..32775.
 *     - TEXTURE: fills texture with COLOR_POISON (0xffff00ff).
 *     - REFERENCE: unpoisoned baseline (no poison written; commanded clear baseline and clean coverage).
 *
 *   Modes:
 *     - reference: baseline unpoisoned (gated: must be 0/32 corruption; all-clear fails).
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

SYS_PROCESS_PARAM(1001, 0x100000);

#ifdef LABEL_REUSE_HEAVY
#include "fp_heavy_nokil_fpo.h"
#define VARIANT             "heavy"
#define RT                  A2P_RT_DIM
#define TEX_W               256u
#define TEX_H               256u
#define TRIALS              32u
#define BACKLOG_PASSES      A2P_BACKLOG_PASSES

#ifdef __powerpc__
static inline uint64_t a2p_read_timebase(void)
{
    unsigned long long tb;
    __asm__ __volatile__(
        "1:\n\t"
        "mftb  %[tb]\n\t"
        "cmpwi 7,%[tb],0\n\t"
        "beq-  7,1b"
        : [tb] "=r"(tb) : : "cr7");
    return (uint64_t)tb;
}
#else
static inline uint64_t a2p_read_timebase(void)
{
    return 0;
}
#endif

#define TILES_X             A2P_TILES_X
#define TILES_Y             A2P_TILES_Y
#define TILES               A2P_TILES           /* 256 tiles */
#define SUBQUADS_X          A2P_SUBQUADS_X
#define SUBQUADS_Y          A2P_SUBQUADS_Y
#define SUBQUADS_PER_TILE   A2P_SUBQUADS_PER_TILE /* 32 sub-quads per tile */
#define TOTAL_SUBQUADS      A2P_TOTAL_SUBQUADS  /* 8,192 sub-quads */
#define TOTAL_SAMPLES       A2P_TOTAL_SAMPLES   /* 8,192 samples */

#define SUBQUAD_W           A2P_SUBQUAD_W       /* 8 pixels */
#define SUBQUAD_H           A2P_SUBQUAD_H       /* 4 pixels */

#define INDICES_PER_TILE    A2P_INDICES_PER_TILE /* 192 indices */
#define TOTAL_INDICES       A2P_TOTAL_INDICES    /* 49,152 indices */

#define ON_TARGET_VERTS     A2P_ON_TARGET_VERTS /* 32,768 vertices (indices 0..32767) */
#define BACKLOG_VERTS_BASE  ON_TARGET_VERTS     /* 4 backlog quad vertices (indices 32768..32771) */
#define OFF_SCREEN_BASE     (BACKLOG_VERTS_BASE + 4u) /* 4 off-screen vertices (indices 32772..32775) */
#define TOTAL_VERTS         A2P_TOTAL_VERTS     /* 32,776 vertices */

#define ARENA_VBO           0x000000u /* 32,776 * 16 = 524,416 bytes (~512 KiB) */
#define ARENA_QUAD_IBO      0x080100u /* 16-bit indices for backlog quad (12 bytes) */
#define ARENA_IBO           0x080180u /* 32-bit indices: 49,152 * 4 = 192 KiB */
#define ARENA_TEX           0x0c0000u /* 256 * 256 * 4 = 256 KiB (ends at 0x100000) */
#define ARENA_READBACK      0x100000u /* 512 * 512 * 4 = 1 MiB (Page 1: 0x100000..0x200000) */
#define ARENA_USED_BYTES    (ARENA_READBACK + RT * RT * 4) /* 0x200000 = 2,097,152 bytes (2 MiB) */
/* gcm_host_buffer_alloc requires 1 MiB multiples; used extent is exactly 2 MiB */
#define ARENA_ALLOC_BYTES   (2u * 1024u * 1024u)
#define ARENA_BYTES         ARENA_ALLOC_BYTES

#define FIFO_BYTES          (1u * 1024u * 1024u)
#define HOST_BYTES          (32u * 1024u * 1024u)
#define POLLS               4000000u
#define SPIN_POLLS          100000u
#define CRUMB_PATH          "/dev_hdd0/tmp/hello-ppu-gcm-label-reuse-heavy.crumb"

_Static_assert(ARENA_VBO + TOTAL_VERTS * sizeof(arena_vertex) <= ARENA_QUAD_IBO, "VBO fits before QUAD_IBO");
_Static_assert(ARENA_QUAD_IBO + 6 * sizeof(uint16_t) <= ARENA_IBO, "QUAD_IBO fits before IBO");
_Static_assert(ARENA_IBO + TOTAL_INDICES * sizeof(uint32_t) <= ARENA_TEX, "IBO fits before TEX");
_Static_assert(ARENA_TEX + TEX_W * TEX_H * sizeof(uint32_t) <= ARENA_READBACK, "TEX fits before READBACK");
_Static_assert(ARENA_READBACK + RT * RT * sizeof(uint32_t) <= ARENA_USED_BYTES, "READBACK fits in used extent");
_Static_assert(ARENA_USED_BYTES <= ARENA_ALLOC_BYTES, "used extent fits in alloc bytes");
_Static_assert((ARENA_ALLOC_BYTES % (1024u * 1024u)) == 0, "ARENA_ALLOC_BYTES must be 1 MiB multiple");
_Static_assert((FIFO_BYTES % (1024u * 1024u)) == 0, "FIFO_BYTES must be 1 MiB multiple");
_Static_assert((HOST_BYTES % (1024u * 1024u)) == 0, "HOST_BYTES must be 1 MiB multiple");
_Static_assert(A2P_REPORT_TS_CMD >= 512u && A2P_REPORT_TS_CMD <= 1023u, "app report index cmd");
_Static_assert(A2P_REPORT_TS_BACKEND >= 512u && A2P_REPORT_TS_BACKEND <= 1023u, "app report index backend");

#include <sys/sys_time.h>

static uint64_t g_session_start_us = 0;

static inline uint64_t a2p_elapsed_us(void)
{
    uint64_t now = (uint64_t)sys_time_get_system_time();
    return now >= g_session_start_us ? now - g_session_start_us : 0;
}

#else /* Light variant */

enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 1 << 20 };
enum { RT = 256, TEX_W = 64, TEX_H = 64, DRAWS = 64, TRIALS = 32, MODES = 4 };
#define ARENA_VBO           0x00000u
#define ARENA_IBO           0x01000u
#define ARENA_TEX           0x10000u
#define ARENA_READBACK      0x40000u
#define POLLS               400000u /* x 5 us: at least a 2 s watchdog per wait, reported as a failure */
#define SPIN_POLLS          100000u
#define WARMUP_MAX          600u
#define VARIANT             "light"
#define CRUMB_PATH          "/dev_hdd0/tmp/gcmlabel-crumb.txt"

_Static_assert(ARENA_READBACK + RT * RT * 4 <= ARENA_BYTES, "readback fits the arena");
_Static_assert(ARENA_TEX % 128 == 0, "texture offset alignment");

static const char *const mode_names[MODES + 1] = {"warmup", "backend", "texture", "command", "early"};
#define mode_name (mode_names + 1) /* mode_name[-1] is the warm-up */

#endif

#define DONE_LABEL          101u
#define READBACK_LABEL      102u
#define COLOR_CLEAR         A2P_COLOR_CLEAR
#define COLOR_POISON        A2P_COLOR_POISON
#define SENTINEL            A2P_SENTINEL

_Static_assert(DONE_LABEL >= CELL_GCM_LABEL_APP_FIRST && READBACK_LABEL <= CELL_GCM_LABEL_APP_LAST, "app labels");

static void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int wait_readback(uint32_t serial)
{
    for (uint32_t i = 0; i < POLLS; ++i) {
#ifdef LABEL_REUSE_HEAVY
        int passed = cellGcmIsLabelPassed(READBACK_LABEL, serial);
        uint64_t elapsed = a2p_elapsed_us();
        int decision = a2p_wait_decision(passed, elapsed);
        if (decision == A2P_WAIT_EXPIRED) {
            crumb("A2P_ABORT deadline elapsed_us=%llu at=readback-wait", (unsigned long long)elapsed);
            return A2P_RC_DEADLINE;
        }
        if (decision == A2P_WAIT_DONE)
            return 0;
#else
        if (cellGcmIsLabelPassed(READBACK_LABEL, serial))
            return 0;
#endif
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
    /* 1. On-target tiles: 256 tiles, each with 32 distinct sub-quads (4x8 grid, 8x4 px each).
     * Total on-target vertices: 256 * 32 * 4 = 32,768 distinct vertices. Zero vertex sharing. */
    for (unsigned t = 0; t < TILES; ++t) {
        unsigned tx = t % TILES_X;
        unsigned ty = t / TILES_Y;
        unsigned tile_px_x = tx * A2P_TILE_CELL_SIZE;
        unsigned tile_px_y = ty * A2P_TILE_CELL_SIZE;

        for (unsigned q = 0; q < SUBQUADS_PER_TILE; ++q) {
            unsigned sx = q % SUBQUADS_X;
            unsigned sy = q / SUBQUADS_X;
            unsigned x_left = tile_px_x + sx * SUBQUAD_W;
            unsigned x_right = x_left + SUBQUAD_W;
            unsigned y_top = tile_px_y + sy * SUBQUAD_H;
            unsigned y_bottom = y_top + SUBQUAD_H;

            /* RSX viewport: Y_screen=0 is NDC +1.0, Y_screen=512 is NDC -1.0 */
            float x0 = -1.0f + 2.0f * ((float)x_left / (float)RT);
            float x1 = -1.0f + 2.0f * ((float)x_right / (float)RT);
            float y1 = 1.0f - 2.0f * ((float)y_top / (float)RT);
            float y0 = 1.0f - 2.0f * ((float)y_bottom / (float)RT);

            float u0 = (float)x_left / (float)RT;
            float u1 = (float)x_right / (float)RT;
            float v0 = (float)y_top / (float)RT;
            float v1 = (float)y_bottom / (float)RT;

            unsigned base = (t * SUBQUADS_PER_TILE + q) * 4;
            vbo[base + 0] = (arena_vertex){{x0, y1}, {u0, v0}};
            vbo[base + 1] = (arena_vertex){{x1, y1}, {u1, v0}};
            vbo[base + 2] = (arena_vertex){{x0, y0}, {u0, v1}};
            vbo[base + 3] = (arena_vertex){{x1, y0}, {u1, v1}};
        }
    }

    /* 2. Full-screen quad for backlog draws (indices 32768..32771) */
    vbo[BACKLOG_VERTS_BASE + 0] = (arena_vertex){{-1.0f,  1.0f}, {0.0f, 0.0f}};
    vbo[BACKLOG_VERTS_BASE + 1] = (arena_vertex){{ 1.0f,  1.0f}, {1.0f, 0.0f}};
    vbo[BACKLOG_VERTS_BASE + 2] = (arena_vertex){{-1.0f, -1.0f}, {0.0f, 1.0f}};
    vbo[BACKLOG_VERTS_BASE + 3] = (arena_vertex){{ 1.0f, -1.0f}, {1.0f, 1.0f}};

    /* 3. Valid in-bounds off-screen vertices for index poison redirection (indices 32772..32775) */
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
        uint32_t *tile_ibo = ibo + t * INDICES_PER_TILE;
        for (unsigned q = 0; q < SUBQUADS_PER_TILE; ++q) {
            uint32_t base_v = (t * SUBQUADS_PER_TILE + q) * 4;
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
        for (unsigned q = 0; q < SUBQUADS_PER_TILE; ++q) {
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
                           uint32_t backlog_rt_depth_off,
                           int mode,
                           int poison_kind,
                           unsigned trial,
                           uint32_t *p_serial,
                           int *out_trial_outcome,
                           int *out_all_clear,
                           uint64_t *out_gap_us)
{
    /* Check deadline before trial */
    uint64_t elapsed = a2p_elapsed_us();
    if (a2p_deadline_expired(elapsed)) {
        crumb("A2P_ABORT deadline elapsed_us=%llu at=pre-trial", (unsigned long long)elapsed);
        return A2P_RC_DEADLINE;
    }

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

    /* Initialise EVERY sampled location (all 8192 sub-quad centres) to SENTINEL before each trial */
    for (unsigned t = 0; t < TILES; ++t) {
        for (unsigned q = 0; q < SUBQUADS_PER_TILE; ++q) {
            size_t off = a2p_subquad_sample_offset(t, q);
            readback[off] = SENTINEL;
        }
    }

    /* Monotonic trial serial */
    uint32_t serial = ++(*p_serial);

    /* 1. Backlog draws: BACKLOG_PASSES full-screen draws of heavy fragment program (fp_heavy_nokil)
     * into a SEPARATE backlog surface to saturate pipelines without touching checked target (~100 ms RSX time).
     * Backlog surface uses its own separate depth buffer; depth test and depth write are explicitly disabled. */
    arena_set_target(ctx, backlog_rt_off, RT * 4, backlog_rt_depth_off, RT * 2, RT, RT);
    cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
    cellGcmSetDepthMask(ctx, GCM_FALSE);
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
    for (unsigned h = 0; h < (mode == MODE_REFERENCE && poison_kind == POISON_NONE ? 0 : BACKLOG_PASSES); ++h) {
        cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 6, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16,
                                 CELL_GCM_LOCATION_MAIN, arena_off + ARENA_QUAD_IBO);
    }

    /* 2. Switch to checked render target; issue COMMANDED baseline clear to pure COLOR_CLEAR
     * including CELL_GCM_CLEAR_Z plus color on the checked surface before tile draws.
     * Depth test and depth write are explicitly disabled for tile draws.
     * (commanded baseline, not a verified observation; no drain or readback inside race window). */
    arena_set_target(ctx, rt_off, RT * 4, rt_depth_off, RT * 2, RT, RT);
    cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
    cellGcmSetDepthMask(ctx, GCM_FALSE);
    cellGcmSetClearColor(ctx, COLOR_CLEAR);
    cellGcmSetClearDepthStencil(ctx, 0xffffff00);
    cellGcmSetClearSurface(ctx, CELL_GCM_CLEAR_Z | CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);

    /* 3. Emit early label if MODE_EARLY */
    if (mode == MODE_EARLY) {
        cellGcmSetTimeStamp(ctx, A2P_REPORT_TS_CMD);
        cellGcmSetWriteCommandLabel(ctx, DONE_LABEL, serial);
    }

    /* 4. Draw 256 non-overlapping tiles with 32 distinct sub-quads per tile */
    cellGcmSetInvalidateVertexCache(ctx);
    cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);
    arena_bind(ctx, prog_main, arena_off + ARENA_VBO, arena_off + ARENA_TEX, TEX_W, TEX_H);

    for (unsigned t = 0; t < TILES; ++t) {
        uint32_t tile_ibo_off = arena_off + ARENA_IBO + t * INDICES_PER_TILE * (uint32_t)sizeof(uint32_t);
        cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES,
                                 INDICES_PER_TILE, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32,
                                 CELL_GCM_LOCATION_MAIN, tile_ibo_off);
    }

    /* 5. Issue completion label for mode under test and measurement timestamps */
    cellGcmSetTimeStamp(ctx, A2P_REPORT_TS_CMD);
    if (mode == MODE_BACKEND || mode == MODE_REFERENCE)
        cellGcmSetWriteBackEndLabel(ctx, DONE_LABEL, serial);
    else if (mode == MODE_TEXTURE)
        cellGcmSetWriteTextureLabel(ctx, DONE_LABEL, serial);
    else if (mode == MODE_COMMAND)
        cellGcmSetWriteCommandLabel(ctx, DONE_LABEL, serial);
    cellGcmSetTimeStamp(ctx, A2P_REPORT_TS_BACKEND);

    /* Publish draw commands and DONE_LABEL; readback transfer is deferred until after poison rewrite */
    cellGcmFlush(ctx);

    /* Check deadline before label wait and poison */
    elapsed = a2p_elapsed_us();
    if (a2p_deadline_expired(elapsed)) {
        crumb("A2P_ABORT deadline elapsed_us=%llu at=pre-poison", (unsigned long long)elapsed);
        return A2P_RC_DEADLINE;
    }

    /* Poll completion label. The deadline is tested first on each poll via a2p_wait_decision().
     * On pass, poison immediately with no intervening work. */
    uint32_t polls = 0;
    uint64_t seen_tb = 0;
    for (;;) {
        int passed = cellGcmIsLabelPassed(DONE_LABEL, serial);
        elapsed = a2p_elapsed_us();
        int decision = a2p_wait_decision(passed, elapsed);
        if (decision == A2P_WAIT_EXPIRED) {
            crumb("A2P_ABORT deadline elapsed_us=%llu at=done-label-wait", (unsigned long long)elapsed);
            return A2P_RC_DEADLINE;
        }
        if (decision == A2P_WAIT_DONE) {
            seen_tb = a2p_read_timebase();
            /* Immediate poison write: no intervening work between pass and poison write */
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
            break;
        }
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

    /* Check deadline before readback */
    elapsed = a2p_elapsed_us();
    if (a2p_deadline_expired(elapsed)) {
        crumb("A2P_ABORT deadline elapsed_us=%llu at=pre-readback", (unsigned long long)elapsed);
        return A2P_RC_DEADLINE;
    }

    /* Issue readback transfer and fence strictly AFTER poison write */
    cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN, arena_off + ARENA_READBACK, RT * 4, 0, 0,
                            rt_off, RT * 4, 0, 0, RT, RT, 4);
    cellGcmSetWriteBackEndLabel(ctx, READBACK_LABEL, serial);
    cellGcmFlush(ctx);

    int rb_rc = wait_readback(serial);
    if (rb_rc != 0) {
        if (rb_rc != A2P_RC_DEADLINE) {
            crumb("A2P_FAIL readback wait mode=%s poison=%s trial=%u",
                  get_mode_str(mode), get_poison_str(poison_kind), trial);
        }
        return rb_rc;
    }

    uint64_t ts_cmd = cellGcmGetTimeStamp(A2P_REPORT_TS_CMD);
    uint64_t ts_backend = cellGcmGetTimeStamp(A2P_REPORT_TS_BACKEND);
    uint64_t gap_us = (ts_backend >= ts_cmd) ? (ts_backend - ts_cmd) / 1000ULL : 0;
    if (out_gap_us)
        *out_gap_us = gap_us;

    /* Check readback pixels using tile classifier across all 8,192 sub-quad centers */
    a2p_tile_report rep;
    int is_ref = (mode == MODE_REFERENCE || poison_kind == POISON_NONE);
    int outcome = a2p_classify_trial_tiles(readback, good, poison_kind, is_ref, &rep);
    *out_trial_outcome = outcome;
    if (out_all_clear)
        *out_all_clear = rep.all_clear;

    /* Debug: for first failing reference trial in a test series, print first 8 failing samples */
    static int s_first_series_ref_fail_printed = 0;
    if (is_ref && poison_kind != POISON_NONE && outcome != A2P_TRIAL_CLEAN && !s_first_series_ref_fail_printed) {
        s_first_series_ref_fail_printed = 1;
        crumb("A2P_SERIES_REF_FAIL mode=%s poison=%s trial=%u good=0x%08x (rgb=0x%06x) all_clear=%d (first 8 failing samples follow)",
              get_mode_str(mode), get_poison_str(poison_kind), trial, good, good & 0x00ffffffu, rep.all_clear);
        unsigned printed = 0;
        for (unsigned t = 0; t < TILES && printed < 8; ++t) {
            for (unsigned q = 0; q < SUBQUADS_PER_TILE && printed < 8; ++q) {
                size_t off = a2p_subquad_sample_offset(t, q);
                uint32_t pix = readback[off];
                if ((pix & 0x00ffffffu) != (good & 0x00ffffffu)) {
                    crumb("A2P_SERIES_REF_FAIL_SAMPLE #%u: tile=%u subquad=%u obs=0x%06x exp_good=0x%06x (full_obs=0x%08x full_good=0x%08x)",
                          printed++, t, q, pix & 0x00ffffffu, good & 0x00ffffffu, pix, good);
                }
            }
        }
    }

    char exp_mask_str[80];
    char unrel_mask_str[80];
    a2p_format_bitmask(rep.expected_mask, exp_mask_str, sizeof exp_mask_str);
    a2p_format_bitmask(rep.unrelated_mask, unrel_mask_str, sizeof unrel_mask_str);

    crumb("A2P_TRIAL mode=%s poison=%s trial=%u label_seen=%u seen_tb=%llu gap_us=%llu poison_order=seen->written->sync "
          "expected=%u unrelated=%u exp_tiles=%u:%s unrel_tiles=%u:%s polls=%u",
          get_mode_str(mode), get_poison_str(poison_kind), trial, seen_val,
          (unsigned long long)seen_tb, (unsigned long long)gap_us,
          rep.expected_tile_count, rep.unrelated_tile_count,
          rep.expected_tile_count, exp_mask_str,
          rep.unrelated_tile_count, unrel_mask_str,
          polls);
    return 0;
}

typedef struct {
    CellGcmContextData *ctx;
    const arena_program *prog_main;
    const arena_program *prog_heavy;
    uint8_t *arena;
    uint32_t arena_off;
    uint32_t rt_off;
    uint32_t backlog_rt_off;
    uint32_t rt_depth_off;
    uint32_t backlog_rt_depth_off;
    uint32_t *p_serial;
    uint64_t gaps[TRIALS];
} a2p_series_ctx;

static int a2p_trial_callback(void *user_ctx, int mode, int poison_kind, unsigned trial,
                              int *out_trial_outcome, int *out_all_clear)
{
    a2p_series_ctx *c = (a2p_series_ctx *)user_ctx;
    uint64_t trial_gap = 0;
    int rc = run_probe_trial(c->ctx, c->prog_main, c->prog_heavy, c->arena, c->arena_off,
                             c->rt_off, c->backlog_rt_off, c->rt_depth_off, c->backlog_rt_depth_off,
                             mode, poison_kind, trial, c->p_serial,
                             out_trial_outcome, out_all_clear, &trial_gap);
    if (trial < TRIALS)
        c->gaps[trial] = trial_gap;
    return rc;
}

static int run_series(CellGcmContextData *ctx,
                      const arena_program *prog_main,
                      const arena_program *prog_heavy,
                      uint8_t *arena,
                      uint32_t arena_off,
                      uint32_t rt_off,
                      uint32_t backlog_rt_off,
                      uint32_t rt_depth_off,
                      uint32_t backlog_rt_depth_off,
                      int mode,
                      int poison_kind,
                      uint32_t *p_serial,
                      a2p_series_counts *out_counts)
{
    a2p_series_ctx sctx = {
        .ctx = ctx,
        .prog_main = prog_main,
        .prog_heavy = prog_heavy,
        .arena = arena,
        .arena_off = arena_off,
        .rt_off = rt_off,
        .backlog_rt_off = backlog_rt_off,
        .rt_depth_off = rt_depth_off,
        .backlog_rt_depth_off = backlog_rt_depth_off,
        .p_serial = p_serial,
        .gaps = {0}
    };

    unsigned failed_trial = 0;
    int rc = a2p_run_series_loop(&sctx, mode, poison_kind, TRIALS,
                                 a2p_trial_callback, out_counts, &failed_trial);
    if (rc != 0) {
        if (rc != A2P_RC_DEADLINE) {
            crumb("A2P_ABORT series mode=%s poison=%s trial=%u",
                  get_mode_str(mode), get_poison_str(poison_kind), failed_trial);
        }
        return rc;
    }
    a2p_gap_stats gap_stats = a2p_compute_gap_stats(sctx.gaps, TRIALS);
    char summary_line[256];
    a2p_format_series_summary(summary_line, sizeof summary_line,
                              get_mode_str(mode), get_poison_str(poison_kind),
                              out_counts, TRIALS, a2p_elapsed_us(), &gap_stats);
    crumb("%s", summary_line);
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
    char reason[128] = {0};
    const char *status = a2p_evaluate_verdict(ref, early, cmd, cand, cand_mode_name,
                                              extra, extra_mode_name, reason, sizeof reason);

    if (out_verdict)
        snprintf(out_verdict, out_verdict_len, "%s", status);

    if (strcmp(status, "supported-for-this-workload") == 0) {
        crumb("A2P VERDICT %-6s %s (%s) [note: 0/32 is not a zero-risk bound]",
              buf_kind, status, reason);
    } else {
        crumb("A2P VERDICT %-6s %s (%s)", buf_kind, status, reason);
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
    g_session_start_us = (uint64_t)sys_time_get_system_time();
    crumb("A2P_SESSION session=%llu (crumb file is append-only; this run's lines follow)",
          (unsigned long long)g_session_start_us);
    crumb("A2P_BEGIN variant=%s target=%ux%u texture=%ux%u tiles=%u subquads=%u vertices=%u indices=%u samples=%u "
          "backlog_passes=%u estimated_session_s=~46s (whole-session average ~100 ms per trial) "
          "deadline_limit_us=%llu deadline_mode=cooperative elapsed_us=%llu",
          VARIANT, RT, RT, TEX_W, TEX_H, TILES, TOTAL_SUBQUADS, TOTAL_VERTS, TOTAL_INDICES, TOTAL_SAMPLES,
          (unsigned)A2P_BACKLOG_PASSES,
          (unsigned long long)A2P_SESSION_LIMIT_US,
          (unsigned long long)a2p_elapsed_us());

    if (gcm_host_buffer_alloc(HOST_BYTES, &host) || cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) ||
        arena_display_init(&d)) {
        crumb("A2P_FAIL init");
        return 1;
    }
    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    void *rt = arena_local_alloc(&d, 64, RT * RT * 4), *rtz = arena_local_alloc(&d, 64, RT * RT * 2);
    void *backlog_rt = arena_local_alloc(&d, 64, RT * RT * 4), *backlog_rtz = arena_local_alloc(&d, 64, RT * RT * 2);
    uint32_t backlog_rt_off = 0, backlog_rt_depth_off = 0;
    if (!rt || !rtz || !backlog_rt || !backlog_rtz || cellGcmAddressToOffset(rt, &rt_off) ||
        cellGcmAddressToOffset(rtz, &rt_depth_off) || cellGcmAddressToOffset(backlog_rt, &backlog_rt_off) ||
        cellGcmAddressToOffset(backlog_rtz, &backlog_rt_depth_off) ||
        gcm_host_buffer_alloc(ARENA_BYTES, &arena_ea) ||
        cellGcmMapMainMemory((const void *)(uintptr_t)arena_ea, ARENA_BYTES, &arena_off) ||
        arena_program_init(&d, &prog, arena_vp_vpo, arena_fp_fpo)) {
        crumb("A2P_FAIL setup");
        return 1;
    }

    uint8_t *arena = (uint8_t *)(uintptr_t)arena_ea;
    arena_program prog_heavy;
    if (arena_program_init(&d, &prog_heavy, arena_vp_vpo, fp_heavy_nokil_fpo)) {
        crumb("A2P_FAIL setup prog_heavy");
        return 1;
    }

    /* Reset label memory to 0 with sync and confirm before any GPU work */
    volatile uint32_t *done_lbl = (volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL);
    volatile uint32_t *readback_lbl = (volatile uint32_t *)cellGcmGetLabelAddress(READBACK_LABEL);
    *done_lbl = 0;
    *readback_lbl = 0;
    __asm__ volatile("sync" ::: "memory");
    if (*done_lbl != 0 || *readback_lbl != 0) {
        crumb("A2P_FAIL label reset failed: done=0x%08x readback=0x%08x", *done_lbl, *readback_lbl);
        return 1;
    }

    uint32_t serial = 0;

    /* Warm-up without poison to ensure shader compilation on emulator is complete */
    unsigned warmup = 0;
    int warm_ok = 0;
    for (unsigned w = 0; w < 30; ++w) {
        uint64_t elapsed = a2p_elapsed_us();
        if (a2p_deadline_expired(elapsed)) {
            crumb("A2P_ABORT deadline elapsed_us=%llu at=warmup", (unsigned long long)elapsed);
            return A2P_RC_DEADLINE;
        }
        int outcome = A2P_TRIAL_CLEAN;
        int all_clear = 0;
        int rc = run_probe_trial(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                                 MODE_REFERENCE, POISON_NONE, w, &serial, &outcome, &all_clear, NULL);
        if (rc != 0) {
            crumb("A2P_ABORT warmup trial=%u", w);
            return rc == A2P_RC_DEADLINE ? A2P_RC_DEADLINE : 1;
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
    int rc;
    if ((rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_REFERENCE, POISON_VERTEX, &serial, &res_ref_v)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_EARLY, POISON_VERTEX, &serial, &res_early_v)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_COMMAND, POISON_VERTEX, &serial, &res_cmd_v)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_BACKEND, POISON_VERTEX, &serial, &res_back_v)) != 0) {
        crumb("A2P_ABORT session vertex");
        return rc == A2P_RC_DEADLINE ? A2P_RC_DEADLINE : 1;
    }
    char v_verdict[128];
    print_buffer_verdict("vertex", res_ref_v, res_early_v, res_cmd_v, res_back_v, "backend",
                         NULL, NULL, v_verdict, sizeof v_verdict);

    /* Buffer kind 2: INDEX poison */
    a2p_series_counts res_ref_i, res_early_i, res_cmd_i, res_back_i, res_tex_i;
    if ((rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_REFERENCE, POISON_INDEX, &serial, &res_ref_i)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_EARLY, POISON_INDEX, &serial, &res_early_i)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_COMMAND, POISON_INDEX, &serial, &res_cmd_i)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_BACKEND, POISON_INDEX, &serial, &res_back_i)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_TEXTURE, POISON_INDEX, &serial, &res_tex_i)) != 0) {
        crumb("A2P_ABORT session index");
        return rc == A2P_RC_DEADLINE ? A2P_RC_DEADLINE : 1;
    }
    char i_verdict[128];
    print_buffer_verdict("index", res_ref_i, res_early_i, res_cmd_i, res_back_i, "backend",
                         &res_tex_i, "texture", i_verdict, sizeof i_verdict);

    /* Buffer kind 3: TEXTURE poison */
    a2p_series_counts res_ref_t, res_early_t, res_cmd_t, res_tex_t, res_back_t;
    if ((rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_REFERENCE, POISON_TEXTURE, &serial, &res_ref_t)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_EARLY, POISON_TEXTURE, &serial, &res_early_t)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_COMMAND, POISON_TEXTURE, &serial, &res_cmd_t)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_TEXTURE, POISON_TEXTURE, &serial, &res_tex_t)) != 0 ||
        (rc = run_series(ctx, &prog, &prog_heavy, arena, arena_off, rt_off, backlog_rt_off, rt_depth_off, backlog_rt_depth_off,
                         MODE_BACKEND, POISON_TEXTURE, &serial, &res_back_t)) != 0) {
        crumb("A2P_ABORT session texture");
        return rc == A2P_RC_DEADLINE ? A2P_RC_DEADLINE : 1;
    }
    char t_verdict[128];
    print_buffer_verdict("texture", res_ref_t, res_early_t, res_cmd_t, res_tex_t, "texture",
                         &res_back_t, "backend", t_verdict, sizeof t_verdict);

    /* Overall verdict evaluation */
    /* Final checkpoint: a session that ran past its budget never reports DONE */
    {
        uint64_t elapsed = a2p_elapsed_us();
        if (a2p_deadline_expired(elapsed)) {
            crumb("A2P_ABORT deadline elapsed_us=%llu at=final", (unsigned long long)elapsed);
            return A2P_RC_DEADLINE;
        }
    }
    char overall[1024];
    if (strcmp(v_verdict, "invalid") == 0 || strcmp(i_verdict, "invalid") == 0 || strcmp(t_verdict, "invalid") == 0) {
        snprintf(overall, sizeof overall, "invalid (subtest failed preconditions: vertex=%s index=%s texture=%s)",
                 v_verdict, i_verdict, t_verdict);
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
    crumb("A2P_END session=%llu elapsed_us=%llu", (unsigned long long)g_session_start_us,
          (unsigned long long)a2p_elapsed_us());
    crumb("A2P DONE session=%llu elapsed_us=%llu", (unsigned long long)g_session_start_us,
          (unsigned long long)a2p_elapsed_us());
    return 0;

#else /* Light variant (LABEL_REUSE_HEAVY not defined) — exact origin/main implementation */

    unsigned corrupt[MODES] = {0}, poison[MODES] = {0}, clear[MODES] = {0};
    uint32_t polls_total[MODES] = {0};

    crumb("GCM_LABEL_REUSE_BEGIN variant=%s modes=%u trials=%u draws=%u target=%ux%u texture=%ux%u", VARIANT, MODES,
          TRIALS, DRAWS, RT, RT, TEX_W, TEX_H);
    if (gcm_host_buffer_alloc(HOST_BYTES, &host) || cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) ||
        arena_display_init(&d)) {
        crumb("GCM_LABEL_REUSE_FAIL init");
        return 1;
    }
    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    void *rt = arena_local_alloc(&d, 64, RT * RT * 4), *rtz = arena_local_alloc(&d, 64, RT * RT * 2);
    if (!rt || !rtz || cellGcmAddressToOffset(rt, &rt_off) || cellGcmAddressToOffset(rtz, &rt_depth_off) ||
        gcm_host_buffer_alloc(ARENA_BYTES, &arena_ea) ||
        cellGcmMapMainMemory((const void *)(uintptr_t)arena_ea, ARENA_BYTES, &arena_off) ||
        arena_program_init(&d, &prog, arena_vp_vpo, arena_fp_fpo)) {
        crumb("GCM_LABEL_REUSE_FAIL setup");
        return 1;
    }
    uint8_t *arena = (uint8_t *)(uintptr_t)arena_ea;
    arena_vertex *vbo = (arena_vertex *)(arena + ARENA_VBO);
    uint16_t *ibo = (uint16_t *)(arena + ARENA_IBO);
    uint32_t *tex = (uint32_t *)(arena + ARENA_TEX);
    volatile uint32_t *readback = (volatile uint32_t *)(arena + ARENA_READBACK);
    static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    memcpy(ibo, idx, sizeof idx);
    cellGcmCgUploadInternalConsts(ctx, prog.vp);

    /* Reset label memory to 0 with sync and confirm before any GPU work */
    volatile uint32_t *done_lbl = (volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL);
    volatile uint32_t *readback_lbl = (volatile uint32_t *)cellGcmGetLabelAddress(READBACK_LABEL);
    *done_lbl = 0;
    *readback_lbl = 0;
    __asm__ volatile("sync" ::: "memory");
    if (*done_lbl != 0 || *readback_lbl != 0) {
        crumb("GCM_LABEL_REUSE_FAIL label reset failed: done=0x%08x readback=0x%08x", *done_lbl, *readback_lbl);
        return 1;
    }

    uint32_t serial = 0;
    /* mode -1 is the warm-up: an emulator that compiles shaders asynchronously skips draws until the program is
     * ready, so back-end trials WITHOUT the poison run until one reads back clean (at most WARMUP_MAX) */
    for (int mode = -1; mode < MODES; ++mode) {
        unsigned ntrials = mode < 0 ? WARMUP_MAX : TRIALS, warmup = 0;
        int warm_ok = 0;
        for (unsigned trial = 0; trial < ntrials; ++trial) {
            uint32_t good = good_color((uint32_t)(mode + 1) * TRIALS + trial % TRIALS);
            /* every earlier trial's readback label passed: the RSX reads nothing of the arena now */
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

            /* reuse the arena the moment the label says so: vertices first (smallest), then the texture */
            /* spin first (react at once), then sleep between polls; finite either way */
            uint32_t polls = 0;
            while (!cellGcmIsLabelPassed(DONE_LABEL, serial)) {
                if (++polls > SPIN_POLLS + POLLS) {
                    crumb("GCM_LABEL_REUSE_FAIL done-label wait mode=%s trial=%u label=0x%08x serial=%u",
                          mode_name[mode], trial, *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL), serial);
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
                crumb("GCM_LABEL_REUSE_FAIL readback wait mode=%s trial=%u", mode_name[mode], trial);
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
                    /* not the clear colour: a fetch read poisoned texels (exactly POISON in the light form, a
                     * max() of GOOD and POISON in the heavy one) */
                    saw_poison += v != (COLOR_CLEAR & 0x00ffffffu);
                    saw_clear += v == (COLOR_CLEAR & 0x00ffffffu);
                }
            }
            if (sentinel) {
                crumb("GCM_LABEL_REUSE_FAIL readback not landed mode=%s trial=%u sentinel=%u", mode_name[mode], trial,
                      sentinel);
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
                crumb("GCM_LABEL_REUSE_CORRUPT mode=%s trial=%u bad_samples=%u poison=%u clear=%u polls=%u",
                      mode_name[mode], trial, bad, saw_poison, saw_clear, polls);
            } else {
                crumb("GCM_LABEL_REUSE_TRIAL mode=%s trial=%u clean polls=%u", mode_name[mode], trial, polls);
            }
        }
        if (mode < 0) {
            if (!warm_ok) {
                crumb("GCM_LABEL_REUSE_FAIL warm-up: no clean trial in %u", WARMUP_MAX);
                return 1;
            }
            crumb("GCM_LABEL_REUSE_WARMUP trials=%u (the last one clean)", warmup);
            continue;
        }
        crumb("GCM_LABEL_REUSE_MODE mode=%s trials=%u corrupt=%u late_texture=%u late_vertex=%u mean_polls=%u",
              mode_name[mode], TRIALS, corrupt[mode], poison[mode], clear[mode], polls_total[mode] / TRIALS);
    }
    int ok = corrupt[0] == 0 && corrupt[1] == 0;
    crumb("%s variant=%s backend=%u/%u texture=%u/%u control_command=%u/%u control_early=%u/%u",
          ok ? "GCM_LABEL_REUSE_OK" : "GCM_LABEL_REUSE_FAIL", VARIANT, corrupt[0], TRIALS, corrupt[1], TRIALS,
          corrupt[2], TRIALS, corrupt[3], TRIALS);
    crumb("GCM_LABEL_REUSE_DETECTOR %s", corrupt[3] ? "sensitive" : "not-sensitive");
    crumb("GCM_LABEL_REUSE_A2 %s", !ok ? "refuted (a backend or texture trial read the reused arena)"
                                   : corrupt[2] ? "conclusive"
                                                : "inconclusive (the command control was not caught)");
    return ok ? 0 : 1;
#endif
}
