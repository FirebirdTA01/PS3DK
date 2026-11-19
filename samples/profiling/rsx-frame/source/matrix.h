/* rsx-frame sample: the per-frame draw matrix. Every frame submits exactly these draws in this order, so whichever
 * frame the capture records, its draw list is the one expected/draws.tsv spells out field by field (keep the two in
 * step: a change here is a change there). */
#ifndef RSXF_SAMPLE_MATRIX_H
#define RSXF_SAMPLE_MATRIX_H

#include <stdint.h>

enum { VP_OFFSET = 0, VP_SCALE = 1, VP_COUNT = 2 };
enum { FP_COLOR = 0, FP_TEX = 1, FP_TEXMOD = 2, FP_COUNT = 3 };
enum { TEX_NONE = -1, TEX_A8R8G8B8 = 0, TEX_R5G6B5 = 1, TEX_B8 = 2, TEX_COUNT = 3 };

/* One cell of the 2 VP x 3 FP matrix: its programs, texture format and fixed-function variant. */
typedef struct matrix_cell {
    const char *label;  /* inside scopes frame/matrix */
    int vp, fp, tex;
    int depth_test;     /* LESS, depth writes on */
    int cull_back;      /* back faces culled (front faces counter-clockwise: the quads stay visible) */
    int blend;          /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD */
    float x, y;         /* the quad's position (NDC) */
} matrix_cell;

static const matrix_cell MATRIX[VP_COUNT * FP_COUNT] = {
    {"vp0_fp0", VP_OFFSET, FP_COLOR,  TEX_NONE,     0, 0, 0, -0.6f,  0.4f},
    {"vp0_fp1", VP_OFFSET, FP_TEX,    TEX_A8R8G8B8, 1, 0, 0,  0.0f,  0.4f},
    {"vp0_fp2", VP_OFFSET, FP_TEXMOD, TEX_R5G6B5,   0, 1, 1,  0.6f,  0.4f},
    {"vp1_fp0", VP_SCALE,  FP_COLOR,  TEX_NONE,     1, 1, 0, -0.6f, -0.1f},
    {"vp1_fp1", VP_SCALE,  FP_TEX,    TEX_B8,       0, 0, 1,  0.0f, -0.1f},
    {"vp1_fp2", VP_SCALE,  FP_TEXMOD, TEX_A8R8G8B8, 1, 1, 1,  0.6f, -0.1f},
};

#define MATRIX_TAG(k) (0x100u + (uint32_t)(k))
#define CLEAR_TAG 0x10u
#define INDEX_TAG 0x200u
#define INSTANCE_TAG 0x300u
#define INSTANCES 8          /* identical draws, only the offset constant differs */
#define DRAWS_PER_FRAME (1 + VP_COUNT * FP_COUNT + 1 + INSTANCES)

#endif
