/* Host-only: prove the PUBLIC cellGcm* forwarders (from <cell/gcm.h> ->
 * gcm_command_c.h) compile against the shared natives and emit the same
 * NV40 words as the native direct calls.  Catches a missing/duplicate
 * native or a forwarder mis-wiring that the native-direct test wouldn't. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cell/gcm.h>   /* CellGcmContextData + cellGcmSet* forwarders */

static void check(const char *lab, const uint32_t *a, const uint32_t *b, int n)
{
    if (memcmp(a, b, sizeof *a * n) != 0) { printf("MISMATCH %s\n", lab); exit(1); }
    printf("ok   %s\n", lab);
}

int main(void)
{
    static uint32_t A[1024], B[1024];
    CellGcmContextData n, r;

    /* blend_func (3 words) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8;
    r.begin=r.current=B; r.end=B+8;
    cellGcmSetBlendFunc(&n, 0x01,0x02,0x03,0x04);
    ps3tc_gcm_state_blend_func(&r, 0x01,0x02,0x03,0x04);
    check("cell==native blend_func", A, B, 3);

    /* blend_equation (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetBlendEquation(&n, 0x2, 0x4);
    ps3tc_gcm_state_blend_equation(&r, 0x2, 0x4);
    check("cell==native blend_equation", A, B, 2);

    /* blend_color (4, two packets) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetBlendColor(&n, 0x11111111u, 0x22222222u);
    ps3tc_gcm_state_blend_color(&r, 0x11111111u, 0x22222222u);
    check("cell==native blend_color", A, B, 4);

    /* blend_enable (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetBlendEnable(&n, 1); ps3tc_gcm_state_blend_enable(&r, 1);
    check("cell==native blend_enable", A, B, 2);

    /* dither_enable (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetDitherEnable(&n, 1); ps3tc_gcm_state_dither_enable(&r, 1);
    check("cell==native dither_enable", A, B, 2);

    /* ---- stencil (2-word) ---- */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetStencilTestEnable(&n, 1); ps3tc_gcm_state_stencil_test_enable(&r, 1);
    check("cell==native stencil_test_enable", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetTwoSidedStencilTestEnable(&n, 1);
    ps3tc_gcm_state_two_sided_stencil_test_enable(&r, 1);
    check("cell==native two_sided_stencil_enable", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetStencilMask(&n, 0xFF); ps3tc_gcm_state_stencil_mask(&r, 0xFF);
    check("cell==native stencil_mask", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetBackStencilMask(&n, 0xFF); ps3tc_gcm_state_back_stencil_mask(&r, 0xFF);
    check("cell==native back_stencil_mask", A, B, 2);

    /* ---- stencil (4-word, count=3 under one header; ref signed int32) ---- */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetStencilFunc(&n, 0x03, (int32_t)0x05, 0xFF);
    ps3tc_gcm_state_stencil_func(&r, 0x03, 0x05, 0xFF);
    check("cell==native stencil_func", A, B, 4);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetStencilOp(&n, 0x150a, 0x1e01, 0x1e02);
    ps3tc_gcm_state_stencil_op(&r, 0x150a, 0x1e01, 0x1e02);
    check("cell==native stencil_op", A, B, 4);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetBackStencilFunc(&n, 0x03, (int32_t)0x05, 0xFF);
    ps3tc_gcm_state_back_stencil_func(&r, 0x03, 0x05, 0xFF);
    check("cell==native back_stencil_func", A, B, 4);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetBackStencilOp(&n, 0x150a, 0x1e01, 0x1e02);
    ps3tc_gcm_state_back_stencil_op(&r, 0x150a, 0x1e01, 0x1e02);
    check("cell==native back_stencil_op", A, B, 4);

    /* ---- raster state (2-word) ---- */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetColorMask(&n, 0x0F); ps3tc_gcm_state_color_mask(&r, 0x0F);
    check("cell==native color_mask", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetColorMaskMrt(&n, 0x0F); ps3tc_gcm_state_color_mask_mrt(&r, 0x0F);
    check("cell==native color_mask_mrt", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetShadeModel(&n, 0x1d00); ps3tc_gcm_state_shade_model(&r, 0x1d00);
    check("cell==native shade_model (cellGcmSetShadeModel)", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetShadeMode(&n, 0x1d00); ps3tc_gcm_state_shade_model(&r, 0x1d00);
    check("cell==native shade_model (cellGcmSetShadeMode)", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetCullFace(&n, 0x03); ps3tc_gcm_state_cull_face(&r, 0x03);
    check("cell==native cull_face", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetFrontFace(&n, 0x01); ps3tc_gcm_state_front_face(&r, 0x01);
    check("cell==native front_face", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetCullFaceEnable(&n, 1); ps3tc_gcm_state_cull_face_enable(&r, 1);
    check("cell==native cull_face_enable", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetFrontPolygonMode(&n, 0x02); ps3tc_gcm_state_front_polygon_mode(&r, 0x02);
    check("cell==native front_polygon_mode", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetBackPolygonMode(&n, 0x02); ps3tc_gcm_state_back_polygon_mode(&r, 0x02);
    check("cell==native back_polygon_mode", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetPolygonOffsetFillEnable(&n, 1);
    ps3tc_gcm_state_polygon_offset_fill_enable(&r, 1);
    check("cell==native polygon_offset_fill_enable", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetLineSmoothEnable(&n, 1); ps3tc_gcm_state_line_smooth_enable(&r, 1);
    check("cell==native line_smooth_enable", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetPolySmoothEnable(&n, 1); ps3tc_gcm_state_polygon_smooth_enable(&r, 1);
    check("cell==native polygon_smooth_enable", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetAlphaTestEnable(&n, 1); ps3tc_gcm_state_alpha_test_enable(&r, 1);
    check("cell==native alpha_test_enable", A, B, 2);

    /* ---- raster state (3-word) ---- */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetAlphaFunc(&n, 0x0210, 0x80);
    ps3tc_gcm_state_alpha_func(&r, 0x0210, 0x80);
    check("cell==native alpha_func", A, B, 3);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetPolygonOffset(&n, 0.5f, -1.5f);
    ps3tc_gcm_state_polygon_offset(&r, 0.5f, -1.5f);
    check("cell==native polygon_offset (float bits)", A, B, 3);

    /* fog_mode (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetFogMode(&n, 2); ps3tc_gcm_state_fog_mode(&r, 2);
    check("cell==native fog_mode", A, B, 2);

    /* fog_params (3, two floats) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetFogParams(&n, 0.5f, 2.5f); ps3tc_gcm_state_fog_params(&r, 0.5f, 2.5f);
    check("cell==native fog_params (floats)", A, B, 3);

    /* anti_aliasing_control (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetAntiAliasingControl(&n, 1, 2, 3, 0xFF);
    ps3tc_gcm_state_anti_aliasing_control(&r, 1, 2, 3, 0xFF);
    check("cell==native anti_aliasing_control", A, B, 2);

    /* user_clip_plane_control (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetUserClipPlaneControl(&n, 1,2,3,4,5,6);
    ps3tc_gcm_state_user_clip_plane_control(&r, 1,2,3,4,5,6);
    check("cell==native user_clip_plane_control", A, B, 2);

    /* fragment_program_gamma_enable (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetFragmentProgramGammaEnable(&n, 1);
    ps3tc_gcm_state_fragment_program_gamma_enable(&r, 1);
    check("cell==native fp_gamma_enable", A, B, 2);

    /* zminmax_control (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZMinMaxControl(&n, 1, 2, 3);
    ps3tc_gcm_state_zminmax_control(&r, 1, 2, 3);
    check("cell==native zminmax_control", A, B, 2);

    /* zminmax_control boundary: public 0x101/0x110/0x1FF are cut to 0x01/0x10/0xFF
       by the forwarder (uint8_t cast, same as the old rsx u8 path).  Guard only —
       the forwarder already narrows, so this passes even before the render_enable fix. */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZMinMaxControl(&n, 0x101, 0x110, 0x1FF);
    ps3tc_gcm_state_zminmax_control(&r, 0x01, 0x10, 0xFF);
    check("cell==native zminmax_control (0x101/0x110/0x1FF -> u8)", A, B, 2);

    /* render_enable (2, unconditional) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetRenderEnable(&n, 0, 3); ps3tc_gcm_state_render_enable(&r, 0, 3);
    check("cell==native render_enable (uncond)", A, B, 2);

    /* render_enable (4, conditional) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetRenderEnable(&n, 2, 3); ps3tc_gcm_state_render_enable(&r, 2, 3);
    check("cell==native render_enable (cond)", A, B, 4);

    /* render_enable boundary: public mode 0x102 / 0xff02 must be cut to 0x02
       (= GCM_CONDITIONAL) and therefore emit the CONDITIONAL 4-word form,
       byte-for-byte equal to native(0x02).  On e5070a543 the native took a
       uint32_t mode, so cellGcmSetRenderEnable(0x102,3) emitted the
       unconditional 2-word form and this check went red; the uint8_t native
       truncates 0x102/0xff02 -> 0x02 and it goes green. */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetRenderEnable(&n, 0x102, 3);
    ps3tc_gcm_state_render_enable(&r, 0x02, 3);
    check("cell==native render_enable (0x102->0x02 cond, 4w)", A, B, 4);
    if (A[0] != (((1u)<<18) | (uint32_t)NV40TCL_NOP)) { printf("MISMATCH render_enable(0x102) not conditional\n"); exit(1); }

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetRenderEnable(&n, 0xff02, 3);
    ps3tc_gcm_state_render_enable(&r, 0x02, 3);
    check("cell==native render_enable (0xff02->0x02 cond, 4w)", A, B, 4);
    if (A[0] != (((1u)<<18) | (uint32_t)NV40TCL_NOP)) { printf("MISMATCH render_enable(0xff02) not conditional\n"); exit(1); }

    /* frequency_divider_operation (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetFrequencyDividerOperation(&n, 0x1234);
    ps3tc_gcm_state_frequency_divider_operation(&r, 0x1234);
    check("cell==native freq_divider", A, B, 2);

    /* ---- viewport (24 words, 3 methods) ---- */
    {
        const float vp_scale[4]  = { 1.5f, 2.5f, 3.5f, 4.5f };
        const float vp_offset[4] = { -1.0f, -2.0f, -3.0f, -4.0f };
        static uint32_t VA[32], VB[32];
        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=VA; n.end=VA+32;
        r.begin=r.current=VB; r.end=VB+32;
        cellGcmSetViewport(&n, 8, 12, 256, 144, 0.5f, 8.25f, vp_scale, vp_offset);
        ps3tc_gcm_state_viewport(&r, 8, 12, 256, 144, 0.5f, 8.25f, vp_scale, vp_offset);
        check("cell==native viewport", VA, VB, 24);
    }

    /* scissor (3 words) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetScissor(&n, 32, 48, 320, 240);
    ps3tc_gcm_state_scissor(&r, 32, 48, 320, 240);
    check("cell==native scissor", A, B, 3);

    /* clear_color (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetClearColor(&n, 0xFF00FF00u);
    ps3tc_gcm_state_clear_color(&r, 0xFF00FF00u);
    check("cell==native clear_color", A, B, 2);

    /* clear_depth_stencil (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetClearDepthStencil(&n, 0x3F800000u);
    ps3tc_gcm_state_clear_depth_stencil(&r, 0x3F800000u);
    check("cell==native clear_depth_stencil", A, B, 2);

    /* zcull_control (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZcullControl(&n, 3, 2);
    ps3tc_gcm_state_zcull_control(&r, 3, 2);
    check("cell==native zcull_control", A, B, 2);

    /* zcull_enable (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZcullEnable(&n, 1, 0);
    ps3tc_gcm_state_zcull_enable(&r, 1, 0);
    check("cell==native zcull_enable (1,0)", A, B, 2);
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZcullEnable(&n, 0, 1);
    ps3tc_gcm_state_zcull_enable(&r, 0, 1);
    check("cell==native zcull_enable (0,1)", A, B, 2);

    /* zcull_limit (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZcullLimit(&n, 0x1111, 0x2222);
    ps3tc_gcm_state_zcull_limit(&r, 0x1111, 0x2222);
    check("cell==native zcull_limit", A, B, 2);

    /* zcull_stats_enable (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZcullStatsEnable(&n, 1);
    ps3tc_gcm_state_zcull_stats_enable(&r, 1);
    check("cell==native zcull_stats_enable", A, B, 2);

    /* scull_control (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetScullControl(&n, 0x21, 0x80, 0x03);
    ps3tc_gcm_state_scull_control(&r, 0x21, 0x80, 0x03);
    check("cell==native scull_control", A, B, 2);

    /* scull_control byte case: cell path func=0x1FF (uint16_t) is cut to 0xFF
       by the forwarder (uint8_t native), exactly as the pre-patch rsx path
       (rsxSetSCullControl(u8 sFunc)) did.  Compare against native 0xFF. */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetScullControl(&n, 0x1FF, 0x80, 0x03);
    ps3tc_gcm_state_scull_control(&r, 0xFF, 0x80, 0x03);
    check("cell==native scull_control (0x1FF->0xFF)", A, B, 2);
    if (A[1] != ((0x03u<<24)|(0x80u<<16)|0xFFu)) { printf("MISMATCH scull byte word\n"); exit(1); }
    printf("ok   scull byte word = 0x%08x (func cut to 0xFF)\n", A[1]);

    /* invalidate_zcull (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetInvalidateZcull(&n);
    ps3tc_gcm_state_invalidate_zcull(&r);
    check("cell==native invalidate_zcull", A, B, 2);

    /* ---- texture-border / attrib-mask / zpass / clear-zcull / ref / caches / vertex-4f / report ---- */
    /* texture_border_color (2, index + color) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetTextureBorderColor(&n, 0x05, 0xA5D1B2C3u);
    ps3tc_gcm_state_texture_border_color(&r, 0x05, 0xA5D1B2C3u);
    check("cell==native texture_border_color (idx 0x05)", A, B, 2);
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetTextureBorderColor(&n, 0x2A, 0x00C0FFB0u);
    ps3tc_gcm_state_texture_border_color(&r, 0x2A, 0x00C0FFB0u);
    check("cell==native texture_border_color (idx 0x2A)", A, B, 2);

    /* vertex_attrib_output_mask (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetVertexAttribOutputMask(&n, 0x0C3A7751u);
    ps3tc_gcm_state_vertex_attrib_output_mask(&r, 0x0C3A7751u);
    check("cell==native vertex_attrib_output_mask", A, B, 2);

    /* zpass_pixel_count_enable (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetZpassPixelCountEnable(&n, 0x64B2E910u);
    ps3tc_gcm_state_zpass_pixel_count_enable(&r, 0x64B2E910u);
    check("cell==native zpass_pixel_count_enable", A, B, 2);

    /* clear_zcull_surface (2, (stencil<<1)|depth) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetClearZcullSurface(&n, 0x5A, 0x27);
    ps3tc_gcm_state_clear_zcull_surface(&r, 0x5A, 0x27);
    check("cell==native clear_zcull_surface (0x5A/0x27)", A, B, 2);
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetClearZcullSurface(&n, 0x81, 0x08);
    ps3tc_gcm_state_clear_zcull_surface(&r, 0x81, 0x08);
    check("cell==native clear_zcull_surface (0x81/0x08)", A, B, 2);

    /* reference_command (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetReferenceCommand(&n, 0x1B8F42A9u);
    ps3tc_gcm_state_reference_command(&r, 0x1B8F42A9u);
    check("cell==native reference_command", A, B, 2);

    /* invalidate_vertex_cache (8) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetInvalidateVertexCache(&n);
    ps3tc_gcm_state_invalidate_vertex_cache(&r);
    check("cell==native invalidate_vertex_cache (8)", A, B, 8);

    /* invalidate_texture_cache (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetInvalidateTextureCache(&n, 0x43CD8E02u);
    ps3tc_gcm_state_invalidate_texture_cache(&r, 0x43CD8E02u);
    check("cell==native invalidate_texture_cache", A, B, 2);

    /* vertex_data_4f (5, v[4] IEEE-32) */
    {
        const float v[4] = { 0.25f, -1.5f, 3.0f, 1000.0f };
        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
        cellGcmSetVertexData4f(&n, 0x2A, v);
        ps3tc_gcm_state_vertex_data_4f(&r, 0x2A, v);
        check("cell==native vertex_data_4f (idx 0x2A)", A, B, 5);
        const float w[4] = { -2.5f, 0.125f, 42.0f, -0.03125f };
        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
        cellGcmSetVertexData4f(&n, 0x07, w);
        ps3tc_gcm_state_vertex_data_4f(&r, 0x07, w);
        check("cell==native vertex_data_4f (idx 0x07)", A, B, 5);
    }

    /* clear_report (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetClearReport(&n, 0x3Du);
    ps3tc_gcm_state_clear_report(&r, 0x3Du);
    check("cell==native clear_report (0x3D)", A, B, 2);

    /* ---- surface / vertex-data-array / VP-constants / update-FP / nop / report-loc ---- */
    /* surface (32 words) */
    {
        static uint32_t sA[64], sB[64];
        const CellGcmSurface surf = {
            .type = 0x01,
            .antiAlias = 0x02,
            .colorFormat = 0x03,
            .colorTarget = 0x04,
            .colorLocation = { 0x00, 0x01, 0x02, 0x03 },
            .colorOffset = { 0x1000, 0x2000, 0x3000, 0x4000 },
            .colorPitch = { 0x0100, 0x0200, 0x0300, 0x0400 },
            .depthFormat = 0x05,
            .depthLocation = 0x01,
            ._pad = { 0, 0 },
            .depthOffset = 0x5000,
            .depthPitch = 0x0500,
            .width = 1024,
            .height = 512,
            .x = 16,
            .y = 32
        };
        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=sA; n.end=sA+64; r.begin=r.current=sB; r.end=sB+64;
        cellGcmSetSurface(&n, &surf);
        ps3tc_gcm_state_surface(&r, &surf);
        check("cell==native surface", sA, sB, 32);
    }

    /* vertex_data_array (4 words) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetVertexDataArray(&n, 0x07, 0x1234, 0x20, 0x04, 0x02, 0x01, 0x00567890);
    ps3tc_gcm_state_vertex_data_array(&r, 0x07, 0x1234, 0x20, 0x04, 0x02, 0x01, 0x00567890);
    check("cell==native vertex_data_array", A, B, 4);

    /* vertex_program_constants (lengths 4 and 36) */
    {
        static uint32_t vpA[64], vpB[64];
        const float c4[4] = { 1.5f, -2.5f, 3.25f, -0.125f };
        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=vpA; n.end=vpA+64; r.begin=r.current=vpB; r.end=vpB+64;
        cellGcmSetVertexProgramConstants(&n, 0x20, 4, c4);
        ps3tc_gcm_state_vertex_program_constants(&r, 0x20, 4, c4);
        check("cell==native vertex_program_constants (len 4)", vpA, vpB, 6);

        float c36[36];
        for (int i = 0; i < 36; i++) c36[i] = (float)(i * 1.5f + 0.25f);
        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=vpA; n.end=vpA+64; r.begin=r.current=vpB; r.end=vpB+64;
        cellGcmSetVertexProgramConstants(&n, 0x40, 36, c36);
        ps3tc_gcm_state_vertex_program_constants(&r, 0x40, 36, c36);
        check("cell==native vertex_program_constants (len 36)", vpA, vpB, 40);
    }

    /* update_fragment_program_parameter (2 words) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetUpdateFragmentProgramParameter(&n, 0x01ABCDEFu);
    ps3tc_gcm_state_update_fragment_program_parameter(&r, 0x01ABCDEFu);
    check("cell==native update_fragment_program_parameter", A, B, 2);

    /* the Unsafe and Location forms of the same update (2 words each); the Location form
     * also checked word for word: FP_ADDRESS, then (location + 1) | 29-bit offset */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetUpdateFragmentProgramParameterUnsafe(&n, 0x01ABCDEFu);
    ps3tc_gcm_state_update_fragment_program_parameter_unsafe(&r, 0x01ABCDEFu);
    check("cell==native update_fragment_program_parameter (unsafe)", A, B, 2);
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetUpdateFragmentProgramParameterLocation(&n, 0xE1ABCDEFu, 1);
    ps3tc_gcm_state_update_fragment_program_location(&r, 0xE1ABCDEFu, 1);
    check("cell==native update_fragment_program_parameter_location", A, B, 2);
    if (A[0] != PS3TC_GCM_METHOD(NV40TCL_FP_ADDRESS, 1) || A[1] != (2u | 0x01ABCDEFu)) {
        printf("MISMATCH update_fragment_program_parameter_location words %08x %08x\n", A[0], A[1]); exit(1);
    }
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetUpdateFragmentProgramParameterLocationUnsafe(&n, 0x00000100u, 0);
    ps3tc_gcm_state_update_fragment_program_location_unsafe(&r, 0x00000100u, 0);
    check("cell==native update_fragment_program_parameter_location (unsafe)", A, B, 2);

    /* nop_command (0, 1 and large count) */
    {
        static uint32_t nopA[64], nopB[64];
        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=nopA; n.end=nopA+64; r.begin=r.current=nopB; r.end=nopB+64;
        cellGcmSetNopCommand(&n, 0);
        ps3tc_gcm_state_nop_command(&r, 0);
        check("cell==native nop_command (0)", nopA, nopB, 0);

        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=nopA; n.end=nopA+64; r.begin=r.current=nopB; r.end=nopB+64;
        cellGcmSetNopCommand(&n, 1);
        ps3tc_gcm_state_nop_command(&r, 1);
        check("cell==native nop_command (1)", nopA, nopB, 1);

        memset(&n,0,sizeof n); memset(&r,0,sizeof r);
        n.begin=n.current=nopA; n.end=nopA+64; r.begin=r.current=nopB; r.end=nopB+64;
        cellGcmSetNopCommand(&n, 16);
        ps3tc_gcm_state_nop_command(&r, 16);
        check("cell==native nop_command (16)", nopA, nopB, 16);
    }

    /* report_location (2 words, cell and rsx location) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetReportLocation(&n, GCM_LOCATION_CELL);
    ps3tc_gcm_state_report_location(&r, GCM_LOCATION_CELL);
    check("cell==native report_location (CELL)", A, B, 2);

    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetReportLocation(&n, GCM_LOCATION_RSX);
    ps3tc_gcm_state_report_location(&r, GCM_LOCATION_RSX);
    check("cell==native report_location (RSX)", A, B, 2);

    /* report (2 words, type and index) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetReport(&n, 0x12, 0x00000034);
    ps3tc_gcm_state_report(&r, 0x12, 0x00000034);
    check("cell==native report", A, B, 2);

    /* transfer_data_mode (3 words) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetTransferDataMode(&n, 0x03);
    ps3tc_gcm_state_transfer_data_mode(&r, 0x03);
    check("cell==native transfer_data_mode", A, B, 3);

    /* transfer_data_offset (8 words) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetTransferDataOffset(&n, 0x12340000, 0x56780000);
    ps3tc_gcm_state_transfer_data_offset(&r, 0x12340000, 0x56780000);
    check("cell==native transfer_data_offset", A, B, 8);

    /* transfer_data_format (6 words) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+64; r.begin=r.current=B; r.end=B+64;
    cellGcmSetTransferDataFormat(&n, 1024, 2048, 512, 256, 2, 4);
    ps3tc_gcm_state_transfer_data_format(&r, 1024, 2048, 512, 256, 2, 4);
    check("cell==native transfer_data_format", A, B, 6);

    puts("PASS (public cell* forwarders == shared natives, byte-for-byte)\n");
    return 0;
}
