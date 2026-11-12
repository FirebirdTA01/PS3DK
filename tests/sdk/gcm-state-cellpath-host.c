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
    static uint32_t A[8], B[8];
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

    /* frequency_divider_operation (2) */
    memset(&n,0,sizeof n); memset(&r,0,sizeof r);
    n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
    cellGcmSetFrequencyDividerOperation(&n, 0x1234);
    ps3tc_gcm_state_frequency_divider_operation(&r, 0x1234);
    check("cell==native freq_divider", A, B, 2);

    puts("PASS (public cell* forwarders == shared natives, byte-for-byte)\n");
    return 0;
}
