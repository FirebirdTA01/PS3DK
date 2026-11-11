/*
 * Host encoding test for the SPU GCM command writer (libgcm_spu_cmd).
 *
 * Every SPU call is run next to its PPU counterpart - the cellGcm* inline
 * calls of <cell/gcm/gcm_command_c.h>, the Cg and texture bridges, and the
 * rsx* emitters of sdk/libgcm_cmd/src/rsx_legacy - with the same arguments,
 * and the word streams must be identical.  For each call the test also
 * checks that
 *   - a measuring SPU context counts exactly the words written,
 *   - a buffer one word too small refuses the whole command, writes
 *     nothing and counts the refused words, and an exact fit succeeds,
 *   - the PSL1GHT spellings (SPU aliases) match the PPU rsx* emitters.
 * The Cg-handle calls are compared through the PPU prepare helpers
 * (<cell/gcm/gcm_spu_prepare.h>) on a synthetic Cg binary.
 *
 * The SPU writer is built with its symbols renamed to spuw_* (see
 * gcm-spu-cmd-test.sh) so both sides link into one program.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/gcm/gcm_spu_cmd.h>          /* renamed: spuw_*, spuw_rsx_* */

#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <rsx/commands.h>
#include <rsx/nv40.h>
typedef gcmContextData CellGcmContextData;
typedef gcmContextCallback CellGcmContextCallback;
typedef struct CellGcmTexture {
    uint8_t format, mipmap, dimension, cubemap;
    uint32_t remap;
    uint16_t width, height, depth;
    uint8_t location, _padding;
    uint32_t pitch, offset;
} CellGcmTexture;
gcmContextData *gCellGcmCurrentContext;
#include <cell/gcm/gcm_cg_bridge.h>
#include <cell/gcm/gcm_tex_bridge.h>
#include <cell/gcm/gcm_spu_prepare.h>

_Static_assert(sizeof(CellGcmSpuTexture) == sizeof(CellGcmTexture), "texture record layouts differ");
_Static_assert(sizeof(CellGcmSpuTexture) == 24, "texture record is 24 bytes");

#define CAP 0x40000u                        /* words; the large draws fit */
static uint32_t ppu_buf[CAP], spu_buf[CAP + 4];
static int failures, cases;

static gcmContextData ppu_ctx(void)
{
    gcmContextData c;
    c.begin = c.current = ppu_buf;
    c.end = ppu_buf + CAP;
    c.callback = NULL;
    return c;
}

/* A test case: two closures that emit the same command on each side. */
typedef void (*PpuFn)(gcmContextData *, const void *);
typedef void (*SpuFn)(CellGcmSpuContextData *, const void *);

static void compare(const char *name, PpuFn ppu, SpuFn spu, const void *arg)
{
    gcmContextData p = ppu_ctx();
    CellGcmSpuContextData s;
    uint32_t n, ok = 1;
    ++cases;
    memset(ppu_buf, 0xcd, sizeof(ppu_buf));
    ppu(&p, arg);
    n = (uint32_t)(p.current - ppu_buf);

    memset(spu_buf, 0xcd, sizeof(spu_buf));
    spuw_SpuSetupContextData(&s, spu_buf, (CAP + 4) * 4u);
    spu(&s, arg);
    if (spuw_SpuGetUsedBytes(&s) != n * 4u || memcmp(spu_buf, ppu_buf, n * 4u) != 0) {
        uint32_t i = 0, sn = spuw_SpuGetUsedBytes(&s) / 4u;
        while (i < n && i < sn && spu_buf[i] == ppu_buf[i]) ++i;
        printf("  FAIL %-40s ppu=%u spu=%u words, first difference at %u (ppu 0x%08x spu 0x%08x)\n",
               name, n, sn, i, i < n ? ppu_buf[i] : 0, i < sn ? spu_buf[i] : 0);
        ok = 0;
    }
    spuw_SpuSetupMeasureContext(&s);
    spu(&s, arg);
    if (spuw_SpuGetUsedBytes(&s) != n * 4u) {
        printf("  FAIL %-40s measured %u bytes, wrote %u\n", name, spuw_SpuGetUsedBytes(&s), n * 4u);
        ok = 0;
    }
    if (n) {
        /* One usable word short (the setup keeps the last word for RETURN). */
        memset(spu_buf, 0xcd, sizeof(spu_buf));
        spuw_SpuSetupContextData(&s, spu_buf, n * 4u);
        spu(&s, arg);
        if (s.current != spu_buf || spu_buf[0] != 0xcdcdcdcdu || spuw_SpuGetOverflowWords(&s) != n ||
            spuw_SpuEnd(&s) != CELL_GCM_SPU_ERROR_OVERFLOW) {
            printf("  FAIL %-40s short buffer: not refused whole (overflow=%u)\n", name,
                   spuw_SpuGetOverflowWords(&s));
            ok = 0;
        }
        spuw_SpuSetupContextData(&s, spu_buf, (n + 1u) * 4u);
        spu(&s, arg);
        if (spuw_SpuGetOverflowWords(&s) || spuw_SpuEnd(&s) != CELL_OK || spu_buf[n] != 0x00020000u ||
            memcmp(spu_buf, ppu_buf, n * 4u) != 0) {
            printf("  FAIL %-40s exact fit refused or wrong\n", name);
            ok = 0;
        }
    }
    if (ok) printf("  ok   %-40s %5u words\n", name, n);
    else ++failures;
}

/* ---- arguments shared by both sides ---- */

static const float SCALE[4] = {640.0f, -360.0f, 0.5f, 0.0f}, OFFS[4] = {640.0f, 360.0f, 0.5f, 0.0f};
static const float V4[4] = {1.0f, -2.5f, 0.125f, 3.0e7f};
static float CONSTS[600];
static const CellGcmTexture TEX = {0x81 /* A8R8G8B8|LN */, 1, 2, 0, 0xaae4, 256, 128, 1, 0, 0, 1024, 0x00400000};

typedef struct { uint32_t a, b, c, d; } U4;

#define PAIR(nm, ppu_body, spu_body) \
    static void nm##_p(gcmContextData *c, const void *a) { const U4 *u = a; (void)u; ppu_body; } \
    static void nm##_s(CellGcmSpuContextData *c, const void *a) { const U4 *u = a; (void)u; spu_body; }

/* cellGcm* PPU calls (gcm_command_c.h spellings resolved to what they
 * forward to) against the SPU cellGcm* calls. */
PAIR(nop,        rsxSetNopCommand(c, 5),                 spuw_SetNopCommand(c, 5))
PAIR(jump,       rsxSetJumpCommand(c, 0x00123400),       spuw_SetJumpCommand(c, 0x00123400))
PAIR(call,       rsxSetCallCommand(c, 0x0ff00040),       spuw_SetCallCommand(c, 0x0ff00040))
PAIR(ret,        rsxSetReturnCommand(c),                 spuw_SetReturnCommand(c))
PAIR(be_label,   rsxSetWriteBackendLabel(c, 201, 0x11223344), spuw_SetWriteBackEndLabel(c, 201, 0x11223344))
PAIR(cmd_label,  rsxSetWriteCommandLabel(c, 202, 0xdeadbeef), spuw_SetWriteCommandLabel(c, 202, 0xdeadbeef))
PAIR(wait_label, rsxSetWaitLabel(c, 203, 7),             spuw_SetWaitLabel(c, 203, 7))
PAIR(clear_col,  rsxSetClearColor(c, 0xff2040c0),        spuw_SetClearColor(c, 0xff2040c0))
PAIR(clear_surf, rsxClearSurface(c, 0xf3),               spuw_SetClearSurface(c, 0xf3))
PAIR(scissor,    rsxSetScissor(c, 16, 32, 320, 200),     spuw_SetScissor(c, 16, 32, 320, 200))
PAIR(viewport,   rsxSetViewport(c, 0, 0, 1280, 720, 0.0f, 1.0f, SCALE, OFFS),
                 spuw_SetViewport(c, 0, 0, 1280, 720, 0.0f, 1.0f, SCALE, OFFS))
PAIR(cmask,      rsxSetColorMask(c, 0x01010101),         spuw_SetColorMask(c, 0x01010101))
PAIR(cmask_mrt,  rsxSetColorMaskMrt(c, 0xf0),            spuw_SetColorMaskMrt(c, 0xf0))
PAIR(blend_en,   rsxSetBlendEnable(c, 1),                spuw_SetBlendEnable(c, 1))
PAIR(blend_fn,   rsxSetBlendFunc(c, 0x302, 0x303, 1, 0), spuw_SetBlendFunc(c, 0x302, 0x303, 1, 0))
PAIR(blend_eq,   rsxSetBlendEquation(c, 0x8006, 0x800a), spuw_SetBlendEquation(c, 0x8006, 0x800a))
PAIR(depth_en,   rsxSetDepthTestEnable(c, 1),            spuw_SetDepthTestEnable(c, 1))
PAIR(depth_mask, rsxSetDepthWriteEnable(c, 0),           spuw_SetDepthMask(c, 0))
PAIR(depth_fn,   rsxSetDepthFunc(c, 0x203),              spuw_SetDepthFunc(c, 0x203))
PAIR(cull_en,    rsxSetCullFaceEnable(c, 1),             spuw_SetCullFaceEnable(c, 1))
PAIR(front,      rsxSetFrontFace(c, 0x901),              spuw_SetFrontFace(c, 0x901))
PAIR(poff_en,    rsxSetPolygonOffsetFillEnable(c, 1),    spuw_SetPolygonOffsetFillEnable(c, 1))
PAIR(poff,       rsxSetPolygonOffset(c, 1.5f, -2.0f),    spuw_SetPolygonOffset(c, 1.5f, -2.0f))
/* cellGcmSetVertexDataArray(index, freq, stride, size, type, loc, offset)
 * forwards to rsxBindVertexArrayAttrib(index, freq, offset, stride, size, type, loc). */
PAIR(vtx_array,  rsxBindVertexArrayAttrib(c, 3, 0, 0x1000, 16, 4, 2, 1),
                 spuw_SetVertexDataArray(c, 3, 0, 16, 4, 2, 1, 0x1000))
PAIR(vtx4f,      rsxDrawVertex4f(c, 7, V4),              spuw_SetVertexData4f(c, 7, V4))
PAIR(freqdiv,    rsxSetFrequencyDividerOperation(c, 0x5), spuw_SetFrequencyDividerOperation(c, 0x5))
PAIR(inval_vtx,  rsxInvalidateVertexCache(c),            spuw_SetInvalidateVertexCache(c))
PAIR(inval_tex,  rsxInvalidateTextureCache(c, 1),        spuw_SetInvalidateTextureCache(c, 1))
PAIR(draw,       rsxDrawVertexArray(c, u->a, u->b, u->c), spuw_SetDrawArrays(c, u->a, u->b, u->c))
/* cellGcmSetDrawIndexArray(mode, count, type, loc, indices) forwards to
 * rsxDrawIndexArray(mode, indices, count, type, loc). */
PAIR(draw_idx,   rsxDrawIndexArray(c, (u8)u->a, u->d, u->b, (u8)u->c, 0),
                 spuw_SetDrawIndexArray(c, (uint8_t)u->a, u->b, (uint8_t)u->c, 0, u->d))
PAIR(vp_consts,  rsxSetVertexProgramConstants(c, u->a, u->b, CONSTS),
                 spuw_SetVertexProgramConstants(c, u->a, u->b, CONSTS))
PAIR(vp_block,   cellGcmSetVertexProgramParameterBlock(c, u->a, u->b, CONSTS),
                 spuw_SetVertexProgramParameterBlock(c, u->a, u->b, CONSTS))
PAIR(fp_update,  rsxUpdateFragmentProgramLocation(c, 0x00012340, GCM_LOCATION_RSX),
                 spuw_SetUpdateFragmentProgramParameter(c, 0x00012340))
PAIR(tex,        ps3tc_gcm_load_texture(c, 2, (const gcmTexture *)&TEX),
                 spuw_SetTexture(c, 2, (const CellGcmSpuTexture *)&TEX))
PAIR(tex_ctl,    ps3tc_gcm_tex_control(c, 3, 1, 0, 0xc00, 2), spuw_SetTextureControl(c, 3, 1, 0, 0xc00, 2))
PAIR(tex_flt,    ps3tc_gcm_tex_filter(c, 4, 0x1fff, 2, 3, 1), spuw_SetTextureFilter(c, 4, 0x1fff, 2, 3, 1))
PAIR(tex_wrap,   ps3tc_gcm_tex_wrap_mode(c, 5, 1, 3, 2, 0, 7, 1),
                 spuw_SetTextureAddress(c, 5, 1, 3, 2, 0, 7, 1))

/* PSL1GHT spellings on the SPU (aliases) against the PPU rsx* emitters. */
PAIR(rsx_blend_fn,  rsxSetBlendFunc(c, 0x302, 0x303, 1, 0), spuw_rsx_SetBlendFunc(c, 0x302, 0x303, 1, 0))
PAIR(rsx_blend_eq,  rsxSetBlendEquation(c, 0x8006, 0x8006), spuw_rsx_SetBlendEquation(c, 0x8006, 0x8006))
PAIR(rsx_scissor,   rsxSetScissor(c, 1, 2, 3, 4),            spuw_rsx_SetScissor(c, 1, 2, 3, 4))
PAIR(rsx_load_tex,  rsxLoadTexture(c, 1, (const gcmTexture *)&TEX),
                    spuw_rsx_LoadTexture(c, 1, (const CellGcmSpuTexture *)&TEX))
PAIR(rsx_tex_ctl,   rsxTextureControl(c, 1, 1, 0, 0xc00, 0), spuw_rsx_TextureControl(c, 1, 1, 0, 0xc00, 0))
PAIR(rsx_tex_flt,   rsxTextureFilter(c, 1, 0, 1, 2, 3),      spuw_rsx_TextureFilter(c, 1, 0, 1, 2, 3))
PAIR(rsx_tex_wrap,  rsxTextureWrapMode(c, 1, 1, 1, 1, 0, 0, 0), spuw_rsx_TextureWrapMode(c, 1, 1, 1, 1, 0, 0, 0))
PAIR(rsx_inval_tex, rsxInvalidateTextureCache(c, 2),         spuw_rsx_InvalidateTextureCache(c, 2))
PAIR(rsx_draw_idx,  rsxDrawIndexArray(c, (u8)u->a, u->d, u->b, (u8)u->c, 1),
                    spuw_rsx_DrawIndexArray(c, (uint8_t)u->a, u->d, u->b, (uint8_t)u->c, 1))

/* ---- a synthetic Cg binary: a vertex and a fragment program ---- */

static uint32_t blob_vp[512], blob_fp[512], vp_ucode[4 * 12];
static CgBinaryParameter *vp_param, *fp_vec, *fp_mat;

static void *carve(uint32_t *blob, uint32_t *used, uint32_t bytes)
{
    void *p = (uint8_t *)blob + *used;
    *used += (bytes + 15u) & ~15u;
    return p;
}

static void build_blobs(void)
{
    uint32_t used = 64;    /* the program header */
    CgBinaryProgram *vp = (CgBinaryProgram *)blob_vp, *fp = (CgBinaryProgram *)blob_fp;
    for (unsigned i = 0; i < sizeof(vp_ucode) / 4; ++i) vp_ucode[i] = 0x40000000u + i * 0x10001u;
    for (unsigned i = 0; i < 600; ++i) CONSTS[i] = (float)i * 0.25f - 7.0f;

    /* VP: 12 instructions at slot 5, one float4x4 uniform at c[8] and one
     * literal-pool constant (default value) at c[467]. */
    CgBinaryVertexProgram *v = carve(blob_vp, &used, sizeof(*v));
    CgBinaryParameter *prm = carve(blob_vp, &used, 2 * sizeof(*prm));
    float *dflt = carve(blob_vp, &used, 16);
    memset(prm, 0, 2 * sizeof(*prm));
    v->instructionCount = 12; v->instructionSlot = 5; v->registerCount = 40;
    v->attributeInputMask = 0x0009; v->attributeOutputMask = 0x4001;
    prm[0].type = CG_FLOAT4x4; prm[0].var = CG_UNIFORM; prm[0].resIndex = 8;
    prm[1].type = CG_FLOAT4; prm[1].var = CG_CONSTANT; prm[1].resIndex = 467; prm[1].paramno = (int32_t)0xfffffffe;
    dflt[0] = 0.0f; dflt[1] = 1.0f; dflt[2] = 0.5f; dflt[3] = 2.0f;
    prm[1].defaultValue = (uint32_t)((uint8_t *)dflt - (uint8_t *)blob_vp);
    vp->program = (uint32_t)((uint8_t *)v - (uint8_t *)blob_vp);
    vp->parameterArray = (uint32_t)((uint8_t *)prm - (uint8_t *)blob_vp);
    vp->parameterCount = 2;
    vp_param = &prm[0];

    /* FP: a float3 uniform patched at three ucode offsets, and a float4x4
     * uniform whose four row leaves are patched at one or two offsets
     * (row 2 unreferenced). */
    used = 64;
    CgBinaryFragmentProgram *f = carve(blob_fp, &used, sizeof(*f));
    CgBinaryParameter *fprm = carve(blob_fp, &used, 6 * sizeof(*fprm));
    uint32_t *e0 = carve(blob_fp, &used, 16), *e1 = carve(blob_fp, &used, 16), *e3 = carve(blob_fp, &used, 16),
             *e4 = carve(blob_fp, &used, 16), *ev = carve(blob_fp, &used, 16);
    memset(f, 0, sizeof(*f)); memset(fprm, 0, 6 * sizeof(*fprm));
    f->instructionCount = 9; f->attributeInputMask = 0x8003; f->texCoordsInputMask = 0x0005;
    f->texCoords2D = 0x0005; f->registerCount = 3; f->outputFromH0 = 0; f->pixelKill = 1;
    ev[0] = 3; ev[1] = 0x10; ev[2] = 0x48; ev[3] = 0x70;
    fprm[0].type = CG_FLOAT3; fprm[0].var = CG_UNIFORM; fprm[0].res = 0x0CB8;
    fprm[0].embeddedConst = (uint32_t)((uint8_t *)ev - (uint8_t *)blob_fp);
    fprm[1].type = CG_FLOAT4x4; fprm[1].var = CG_UNIFORM; fprm[1].res = 0x0CB8;
    e0[0] = 1; e0[1] = 0x20; e1[0] = 2; e1[1] = 0x30; e1[2] = 0x90; e3[0] = 1; e3[1] = 0xa0;
    (void)e4;
    uint32_t *rows[4] = {e0, e1, NULL, e3};
    for (int r = 0; r < 4; ++r) {
        fprm[2 + r].type = CG_FLOAT4; fprm[2 + r].var = 0x1006; fprm[2 + r].res = 0x0CB8;
        fprm[2 + r].embeddedConst = rows[r] ? (uint32_t)((uint8_t *)rows[r] - (uint8_t *)blob_fp) : 0u;
    }
    fp->program = (uint32_t)((uint8_t *)f - (uint8_t *)blob_fp);
    fp->parameterArray = (uint32_t)((uint8_t *)fprm - (uint8_t *)blob_fp);
    fp->parameterCount = 6;
    fp_vec = &fprm[0]; fp_mat = &fprm[1];
}

static uint32_t prepared[4096], prepared_words;
static CellGcmSpuVertexParameter vparam;
static CellGcmSpuFragmentParameter fparam_vec, fparam_mat;

PAIR(vp_prog,  cellGcmSetVertexProgram(c, (CGprogram)blob_vp, vp_ucode),
               spuw_SetVertexProgram(c, prepared, prepared_words))
PAIR(fp_prog,  cellGcmSetFragmentProgram(c, (CGprogram)blob_fp, 0x00004000),
               spuw_SetFragmentProgram(c, prepared, prepared_words))
PAIR(vp_param, cellGcmSetVertexProgramParameter(c, (CGparameter)vp_param, CONSTS + 4),
               spuw_SetVertexProgramParameter(c, &vparam, CONSTS + 4))
PAIR(fp_pvec,  cellGcmSetFragmentProgramParameter(c, (CGprogram)blob_fp, (CGparameter)fp_vec, CONSTS + 9, 0x00004000),
               spuw_SetFragmentProgramParameter(c, &fparam_vec, CONSTS + 9, 0x00004000))
PAIR(fp_pmat,  cellGcmSetFragmentProgramParameter(c, (CGprogram)blob_fp, (CGparameter)fp_mat, CONSTS + 20, 0x00004010),
               spuw_SetFragmentProgramParameter(c, &fparam_mat, CONSTS + 20, 0x00004010))

#define RUN(nm, arg) compare(#nm, nm##_p, nm##_s, arg)

/* ---- accounting and refusal checks (not comparisons) ---- */

static int checks, check_failures;
#define EXPECT(nm, cond) do { ++checks; if (cond) printf("  ok   %s\n", nm); \
    else { printf("  FAIL %s: %s (line %d)\n", nm, #cond, __LINE__); ++check_failures; } } while (0)

/* Refusals are sticky and every count saturates instead of wrapping. */
static void test_accounting(void)
{
    static uint32_t small[16], zero[4];
    CellGcmSpuContextData c;

    puts("accounting:");
    spuw_SpuSetupContextData(&c, small, sizeof(small));
    EXPECT("refuse 0x80000000 words", spuw_SpuReserve(&c, 0x80000000u) == NULL);
    EXPECT("refuse 0x80000000 words again", spuw_SpuReserve(&c, 0x80000000u) == NULL);
    EXPECT("two refusals saturate overflow", spuw_SpuGetOverflowWords(&c) == 0xffffffffu);
    EXPECT("later fit still written", spuw_SpuReserve(&c, 2) == small);
    EXPECT("end after two refusals", spuw_SpuEnd(&c) == CELL_GCM_SPU_ERROR_OVERFLOW);

    spuw_SpuSetupMeasureContext(&c);
    spuw_SpuReserve(&c, 0x80000000u);
    spuw_SpuReserve(&c, 0x80000000u);
    EXPECT("measured words saturate", c.measuredWords == 0xffffffffu);
    EXPECT("measured bytes saturate", spuw_SpuGetUsedBytes(&c) == 0xffffffffu);
    spuw_SpuSetupMeasureContext(&c);
    spuw_SpuReserve(&c, 0x40000000u);       /* 2^32 bytes */
    EXPECT("byte conversion saturates", spuw_SpuGetUsedBytes(&c) == 0xffffffffu);

    /* A non-null buffer with no room even for the RETURN. */
    for (uint32_t size = 0; size < 4u; ++size) {
        memset(zero, 0xcd, sizeof(zero));
        spuw_SpuSetupContextData(&c, zero, size);
        spuw_SetNopCommand(&c, 1);
        EXPECT("zero-byte context refuses commands", c.current == zero && spuw_SpuGetOverflowWords(&c) != 0);
        EXPECT("zero-byte context refuses end", spuw_SpuEnd(&c) == CELL_GCM_SPU_ERROR_OVERFLOW);
        EXPECT("zero-byte end writes no RETURN", zero[0] == 0xcdcdcdcdu && c.current == zero);
        EXPECT("zero-byte used bytes", spuw_SpuGetUsedBytes(&c) == 0);
        EXPECT("zero-byte second end", spuw_SpuEnd(&c) != CELL_OK && zero[0] == 0xcdcdcdcdu);
    }
    /* The smallest real buffer: one word, the RETURN. */
    spuw_SpuSetupContextData(&c, zero, 4);
    EXPECT("one-word context ends", spuw_SpuEnd(&c) == CELL_OK && zero[0] == 0x00020000u);
}

int main(void)
{
    static const U4 draws[] = {
        {5, 0, 3, 0}, {5, 10, 256, 0}, {5, 0, 257, 0}, {6, 7, 0x7ff00 + 0x100 * 3 + 9, 0},
        {5, 0, 0x7ff * 0x100 * 2 + 300, 0},
    };
    static const U4 idx[] = {   /* mode, count, type, indices offset */
        {5, 3, 1, 0x1000}, {5, 300, 0, 0x1004}, {4, 1000, 1, 0x1006}, {5, 0x7ff00 * 2 + 77, 0, 0x2010},
        {5, 2, 1, 0x1002}, {5, 256, 1, 0x1080},
    };
    static const U4 consts[] = {{0, 4, 0, 0}, {8, 32, 0, 0}, {3, 70, 0, 0}, {100, 128, 0, 0}};
    static const U4 blocks[] = {{0, 1, 0, 0}, {8, 8, 0, 0}, {20, 13, 0, 0}};
    static const U4 none = {0, 0, 0, 0};

    puts("commands:");
    RUN(nop, &none); RUN(jump, &none); RUN(call, &none); RUN(ret, &none);
    RUN(be_label, &none); RUN(cmd_label, &none); RUN(wait_label, &none);
    RUN(clear_col, &none); RUN(clear_surf, &none); RUN(scissor, &none); RUN(viewport, &none);
    RUN(cmask, &none); RUN(cmask_mrt, &none); RUN(blend_en, &none); RUN(blend_fn, &none);
    RUN(blend_eq, &none); RUN(depth_en, &none); RUN(depth_mask, &none); RUN(depth_fn, &none);
    RUN(cull_en, &none); RUN(front, &none); RUN(poff_en, &none); RUN(poff, &none);
    RUN(vtx_array, &none); RUN(vtx4f, &none); RUN(freqdiv, &none); RUN(inval_vtx, &none);
    RUN(inval_tex, &none); RUN(fp_update, &none);
    RUN(tex, &none); RUN(tex_ctl, &none); RUN(tex_flt, &none); RUN(tex_wrap, &none);
    for (unsigned i = 0; i < sizeof(draws) / sizeof(draws[0]); ++i) RUN(draw, &draws[i]);
    for (unsigned i = 0; i < sizeof(idx) / sizeof(idx[0]); ++i) RUN(draw_idx, &idx[i]);
    for (unsigned i = 0; i < sizeof(consts) / sizeof(consts[0]); ++i) RUN(vp_consts, &consts[i]);
    for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); ++i) RUN(vp_block, &blocks[i]);

    puts("PSL1GHT spellings (SPU aliases vs PPU rsx*):");
    RUN(rsx_blend_fn, &none); RUN(rsx_blend_eq, &none); RUN(rsx_scissor, &none);
    RUN(rsx_load_tex, &none); RUN(rsx_tex_ctl, &none); RUN(rsx_tex_flt, &none);
    RUN(rsx_tex_wrap, &none); RUN(rsx_inval_tex, &none);
    for (unsigned i = 0; i < sizeof(idx) / sizeof(idx[0]); ++i) RUN(rsx_draw_idx, &idx[i]);

    puts("Cg-handle calls through the PPU prepare helpers:");
    build_blobs();
    if (cellGcmSpuPrepareVertexProgram((CGprogram)blob_vp, vp_ucode, prepared, 4096, &prepared_words)) {
        puts("  FAIL prepare vertex program"); ++failures;
    } else RUN(vp_prog, &none);
    {
        uint32_t tiny[8], w;
        if (cellGcmSpuPrepareVertexProgram((CGprogram)blob_vp, vp_ucode, tiny, 8, &w) != -1) {
            puts("  FAIL prepare accepted an 8-word buffer"); ++failures;
        } else puts("  ok   prepare refuses a buffer that is too small");
    }
    if (cellGcmSpuPrepareFragmentProgram((CGprogram)blob_fp, 0x00004000, prepared, 4096, &prepared_words)) {
        puts("  FAIL prepare fragment program"); ++failures;
    } else RUN(fp_prog, &none);
    if (cellGcmSpuPrepareVertexParameter((CGparameter)vp_param, &vparam)) { puts("  FAIL prepare vp param"); ++failures; }
    else RUN(vp_param, &none);
    if (cellGcmSpuPrepareFragmentParameter((CGprogram)blob_fp, (CGparameter)fp_vec, &fparam_vec) ||
        cellGcmSpuPrepareFragmentParameter((CGprogram)blob_fp, (CGparameter)fp_mat, &fparam_mat)) {
        puts("  FAIL prepare fp params"); ++failures;
    } else {
        RUN(fp_pvec, &none); RUN(fp_pmat, &none);
        printf("  fp patches: vector %u, matrix %u\n", fparam_vec.patchCount, fparam_mat.patchCount);
        if (fparam_vec.patchCount != 3 || fparam_mat.patchCount != 4) { puts("  FAIL patch counts"); ++failures; }
    }

    test_accounting();

    if (failures || check_failures) {
        printf("gcm-spu-cmd: %d of %d case(s) and %d of %d check(s) FAILED\n", failures, cases,
               check_failures, checks);
        return 1;
    }
    printf("gcm-spu-cmd: all %d cases byte-identical, all %d checks passed\n", cases, checks);
    return 0;
}
