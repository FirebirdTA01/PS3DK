/* hello-ppu-gcm-label-reuse: is a completion label written late enough to reuse the buffers a frame read?
 *
 * Each trial renders DRAWS full-target quads into a 256x256 local-memory target.  Their vertices, indices and their
 * one texture live in a cellGcmMapMainMemory'd arena (CELL_GCM_LOCATION_MAIN), filled with a per-trial GOOD colour.
 * After the last draw the trial's completion label (application label 101) is written by the mode under test.  The
 * PPU polls it with cellGcmIsLabelPassed and, the moment it has passed, overwrites the arena: the texture with
 * POISON and the vertices with a quad outside the target.  The target is then copied to main memory by an image
 * transfer that a second label (102, back-end) completes, and checked: any pixel that is not GOOD means a draw read
 * the arena after the label said it was free (POISON: a late texture read; CLEAR: a late vertex or index fetch).
 *
 * Modes (TRIALS each):
 *   backend   cellGcmSetWriteBackEndLabel after the last draw      (the reuse rule under test)
 *   texture   cellGcmSetWriteTextureLabel after the last draw      (the texture-read label)
 *   command   cellGcmSetWriteCommandLabel after the last draw      CONTROL: written when the front end reaches it
 *   early     cellGcmSetWriteCommandLabel BEFORE the first draw    CONTROL: certainly too early
 * The controls measure whether this platform can show the hazard at all: a platform whose controls report no
 * corruption cannot confirm or refute the rule for the other two modes (see the README).
 *
 * Output: GCM_LABEL_REUSE_* lines (also appended, fsync'd, to /dev_hdd0/tmp/gcmlabel-crumb.txt); the verdict line
 * is GCM_LABEL_REUSE_OK (backend and texture modes clean) or GCM_LABEL_REUSE_FAIL, then
 * GCM_LABEL_REUSE_DETECTOR sensitive|not-sensitive (the early control) and GCM_LABEL_REUSE_A2 conclusive|inconclusive:
 * conclusive only when the command control is caught, i.e. the workload is heavy enough that a label written by the
 * front end is too early, and the backend and texture modes still stay clean.  Built twice: this light form, and
 * hello-ppu-gcm-label-reuse-heavy (LABEL_REUSE_HEAVY: a larger texture, target and draw count, 8-tap fetches). */
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

SYS_PROCESS_PARAM(1001, 0x100000);

#ifdef LABEL_REUSE_HEAVY
/* The heavy variant (hello-ppu-gcm-label-reuse-heavy): a 1024x1024 texture (4 MiB), 256 draws over a 512x512
 * target and an 8-tap fragment program, so the RSX is still fetching from the arena long after the front end has
 * passed a command label.  Bounds scale with the work: every wait stays finite and is reported as a failure. */
enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 8 << 20 };
enum { RT = 512, TEX_W = 1024, TEX_H = 1024, DRAWS = 256, TRIALS = 32, MODES = 4 };
#define ARENA_TEX 0x100000u
#define ARENA_READBACK 0x600000u
#define POLLS 4000000u /* x 5 us: at least a 20 s watchdog per wait */
#define VARIANT "heavy"
#else
enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 1 << 20 };
enum { RT = 256, TEX_W = 64, TEX_H = 64, DRAWS = 64, TRIALS = 32, MODES = 4 };
#define ARENA_TEX 0x10000u
#define ARENA_READBACK 0x40000u
#define POLLS 400000u /* x 5 us: at least a 2 s watchdog per wait, reported as a failure */
#define VARIANT "light"
#endif
#define DONE_LABEL 101u     /* the trial's completion label, written by the mode under test */
#define READBACK_LABEL 102u /* back-end label after the readback transfer */
#define ARENA_VBO 0x00000u
#define ARENA_IBO 0x01000u
#define COLOR_POISON 0xffff00ffu
#define COLOR_CLEAR 0xff000000u
#define SENTINEL 0x5a5a5a5au
#define WARMUP_MAX 600u /* unchecked warm-up trials at most */
#define CRUMB_PATH "/dev_hdd0/tmp/gcmlabel-crumb.txt"
#define SPIN_POLLS 100000u /* label polls without sleeping before the POLLS x 5 us bound starts */

_Static_assert(DONE_LABEL >= CELL_GCM_LABEL_APP_FIRST && READBACK_LABEL <= CELL_GCM_LABEL_APP_LAST, "app labels");
_Static_assert(ARENA_READBACK + RT * RT * 4 <= ARENA_BYTES, "readback fits the arena");
_Static_assert(ARENA_TEX % 128 == 0, "texture offset alignment");

static const char *const mode_names[MODES + 1] = {"warmup", "backend", "texture", "command", "early"};
#define mode_name (mode_names + 1) /* mode_name[-1] is the warm-up */

static int wait_readback(uint32_t serial)
{
    for (uint32_t i = 0; i < POLLS; ++i) {
        if (cellGcmIsLabelPassed(READBACK_LABEL, serial))
            return 0;
        usleep(5);
    }
    return -1;
}

/* One progress line, appended and fsync'd before returning, so a console that hangs still leaves the last step on
 * disk.  Best effort: a crumb that cannot be written never changes the verdict. */
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

static uint32_t good_color(uint32_t trial)
{
    /* distinct per trial, never POISON, CLEAR or SENTINEL in the low 24 bits */
    return 0xff000000u | (0x204060u + trial * 0x030507u);
}

int main(void)
{
    arena_display d;
    arena_program prog;
    sys_addr_t host = 0, arena_ea = 0;
    uint32_t arena_off = 0, rt_off = 0, rt_depth_off = 0;
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
    *(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL) = 0u;
    *(volatile uint32_t *)cellGcmGetLabelAddress(READBACK_LABEL) = 0u;
    cellGcmCgUploadInternalConsts(ctx, prog.vp);

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
}
