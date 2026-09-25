#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <cell/sysmodule.h>
#include <jpgdec/jpgdec.h>
#include <pngdec/pngdec.h>

SYS_PROCESS_PARAM(1001, 0x100000);

/* Embedded images via ps3_bin2s */
extern const uint8_t  test_jpg[] __attribute__((aligned(4)));
extern const uint32_t test_jpg_size;

extern const uint8_t  test_rgba_png[] __attribute__((aligned(4)));
extern const uint32_t test_rgba_png_size;

extern const uint8_t  test_rgb_png[] __attribute__((aligned(4)));
extern const uint32_t test_rgb_png_size;

static int failures = 0;

static void check(const char *name, int condition)
{
    if (condition) {
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

static inline int abs_diff(int a, int b)
{
    return a > b ? a - b : b - a;
}

int main(void)
{
    printf("=== legacy-image-dec regression test ===\n");

    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC);
    check("cellSysmoduleLoadModule(JPGDEC)", rc == 0);

    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);
    check("cellSysmoduleLoadModule(PNGDEC)", rc == 0);

    /* -------------------------------------------------------------
     * 1. JPEG from Buffer (solid block, host-computed expected RGB)
     * Host PIL decoded (32,32): R=0x23 (35), G=0x68 (104), B=0xab (171)
     * ------------------------------------------------------------- */
    printf("\n--- Test 1: jpgLoadFromBuffer ---\n");
    jpgData jdata_buf;
    memset(&jdata_buf, 0, sizeof(jdata_buf));
    int32_t jret = jpgLoadFromBuffer(test_jpg, test_jpg_size, &jdata_buf);
    printf("  jpgLoadFromBuffer ret=0x%08x width=%u height=%u pitch=%u bmp=%p\n",
           (unsigned)jret, (unsigned)jdata_buf.width, (unsigned)jdata_buf.height,
           (unsigned)jdata_buf.pitch, jdata_buf.bmp_out);
    check("jpgLoadFromBuffer return code == 0", jret == 0);
    check("jpgLoadFromBuffer width == 64", jdata_buf.width == 64);
    check("jpgLoadFromBuffer height == 64", jdata_buf.height == 64);
    check("jpgLoadFromBuffer pitch == 256", jdata_buf.pitch == 256);
    check("jpgLoadFromBuffer bmp_out != NULL", jdata_buf.bmp_out != NULL);

    uint32_t j_buf_mid = 0;
    if (jdata_buf.bmp_out) {
        uint32_t *pixels = (uint32_t *)jdata_buf.bmp_out;
        j_buf_mid = pixels[(32 * 64) + 32];
        uint8_t a = (j_buf_mid >> 24) & 0xff;
        uint8_t r = (j_buf_mid >> 16) & 0xff;
        uint8_t g = (j_buf_mid >> 8) & 0xff;
        uint8_t b = j_buf_mid & 0xff;
        printf("  JPEG pixel(32,32)=0x%08x (A=0x%02x R=0x%02x G=0x%02x B=0x%02x)\n",
               (unsigned)j_buf_mid, a, r, g, b);
        check("jpgLoadFromBuffer alpha == 0xff", a == 0xff);
        check("jpgLoadFromBuffer R channel within 8 of host 0x23", abs_diff(r, 0x23) <= 8);
        check("jpgLoadFromBuffer G channel within 8 of host 0x68", abs_diff(g, 0x68) <= 8);
        check("jpgLoadFromBuffer B channel within 8 of host 0xab", abs_diff(b, 0xab) <= 8);
        free(jdata_buf.bmp_out);
    }

    /* -------------------------------------------------------------
     * 2. JPEG from File (single proven path: /app_home/data/test.jpg)
     * ------------------------------------------------------------- */
    printf("\n--- Test 2: jpgLoadFromFile ---\n");
    jpgData jdata_file;
    memset(&jdata_file, 0, sizeof(jdata_file));
    int32_t jf_ret = jpgLoadFromFile("/app_home/data/test.jpg", &jdata_file);
    printf("  jpgLoadFromFile ret=0x%08x width=%u height=%u pitch=%u bmp=%p\n",
           (unsigned)jf_ret, (unsigned)jdata_file.width, (unsigned)jdata_file.height,
           (unsigned)jdata_file.pitch, jdata_file.bmp_out);
    check("jpgLoadFromFile return code == 0", jf_ret == 0);
    check("jpgLoadFromFile width == 64", jdata_file.width == 64);
    check("jpgLoadFromFile height == 64", jdata_file.height == 64);
    check("jpgLoadFromFile pitch == 256", jdata_file.pitch == 256);
    check("jpgLoadFromFile bmp_out != NULL", jdata_file.bmp_out != NULL);

    if (jdata_file.bmp_out) {
        uint32_t *pixels = (uint32_t *)jdata_file.bmp_out;
        uint32_t p_mid = pixels[(32 * 64) + 32];
        printf("  file pixel(32,32)=0x%08x\n", (unsigned)p_mid);
        check("jpgLoadFromFile pixel matches buffer", p_mid == j_buf_mid);
        free(jdata_file.bmp_out);
    }

    /* -------------------------------------------------------------
     * 3. RGBA PNG from Buffer (distinct R, G, B and alpha 0x80 preserved)
     * ------------------------------------------------------------- */
    printf("\n--- Test 3: pngLoadFromBuffer (RGBA) ---\n");
    pngData pdata_rgba_buf;
    memset(&pdata_rgba_buf, 0, sizeof(pdata_rgba_buf));
    int32_t pret_rgba = pngLoadFromBuffer(test_rgba_png, test_rgba_png_size, &pdata_rgba_buf);
    printf("  pngLoadFromBuffer (RGBA) ret=0x%08x width=%u height=%u pitch=%u bmp=%p\n",
           (unsigned)pret_rgba, (unsigned)pdata_rgba_buf.width, (unsigned)pdata_rgba_buf.height,
           (unsigned)pdata_rgba_buf.pitch, pdata_rgba_buf.bmp_out);
    check("pngLoadFromBuffer (RGBA) return code == 0", pret_rgba == 0);
    check("pngLoadFromBuffer (RGBA) width == 8", pdata_rgba_buf.width == 8);
    check("pngLoadFromBuffer (RGBA) height == 8", pdata_rgba_buf.height == 8);
    check("pngLoadFromBuffer (RGBA) pitch == 32", pdata_rgba_buf.pitch == 32);
    check("pngLoadFromBuffer (RGBA) bmp_out != NULL", pdata_rgba_buf.bmp_out != NULL);

    if (pdata_rgba_buf.bmp_out) {
        uint32_t *pixels = (uint32_t *)pdata_rgba_buf.bmp_out;
        printf("  pixel(0,0)=0x%08x, pixel(1,0)=0x%08x, pixel(2,0)=0x%08x, pixel(3,0)=0x%08x\n",
               (unsigned)pixels[0], (unsigned)pixels[1], (unsigned)pixels[2], (unsigned)pixels[3]);
        /* Exact ARGB words:
         * (0,0): RGBA=(0x12, 0x34, 0x56, 0x80) -> ARGB = 0x80123456 (alpha 0x80 preserved)
         * (1,0): RGBA=(0xfe, 0xdc, 0xba, 0xff) -> ARGB = 0xfffedcba
         * (2,0): RGBA=(0xaa, 0x55, 0xcc, 0x40) -> ARGB = 0x40aa55cc
         * (3,0): RGBA=(0x70, 0x80, 0x90, 0x20) -> ARGB = 0x20708090
         */
        check("pngLoadFromBuffer (RGBA) pixel(0,0) exact ARGB 0x80123456", pixels[0] == 0x80123456);
        check("pngLoadFromBuffer (RGBA) pixel(1,0) exact ARGB 0xfffedcba", pixels[1] == 0xfffedcba);
        check("pngLoadFromBuffer (RGBA) pixel(2,0) exact ARGB 0x40aa55cc", pixels[2] == 0x40aa55cc);
        check("pngLoadFromBuffer (RGBA) pixel(3,0) exact ARGB 0x20708090", pixels[3] == 0x20708090);
        free(pdata_rgba_buf.bmp_out);
    }

    /* -------------------------------------------------------------
     * 4. RGBA PNG from File (/app_home/data/test_rgba.png)
     * ------------------------------------------------------------- */
    printf("\n--- Test 4: pngLoadFromFile (RGBA) ---\n");
    pngData pdata_rgba_file;
    memset(&pdata_rgba_file, 0, sizeof(pdata_rgba_file));
    int32_t pfret_rgba = pngLoadFromFile("/app_home/data/test_rgba.png", &pdata_rgba_file);
    printf("  pngLoadFromFile (RGBA) ret=0x%08x width=%u height=%u pitch=%u bmp=%p\n",
           (unsigned)pfret_rgba, (unsigned)pdata_rgba_file.width, (unsigned)pdata_rgba_file.height,
           (unsigned)pdata_rgba_file.pitch, pdata_rgba_file.bmp_out);
    check("pngLoadFromFile (RGBA) return code == 0", pfret_rgba == 0);
    check("pngLoadFromFile (RGBA) width == 8", pdata_rgba_file.width == 8);
    check("pngLoadFromFile (RGBA) height == 8", pdata_rgba_file.height == 8);
    check("pngLoadFromFile (RGBA) pitch == 32", pdata_rgba_file.pitch == 32);
    check("pngLoadFromFile (RGBA) bmp_out != NULL", pdata_rgba_file.bmp_out != NULL);

    if (pdata_rgba_file.bmp_out) {
        uint32_t *pixels = (uint32_t *)pdata_rgba_file.bmp_out;
        printf("  file pixel(0,0)=0x%08x, pixel(1,0)=0x%08x\n",
               (unsigned)pixels[0], (unsigned)pixels[1]);
        check("pngLoadFromFile (RGBA) pixel(0,0) exact ARGB 0x80123456", pixels[0] == 0x80123456);
        check("pngLoadFromFile (RGBA) pixel(1,0) exact ARGB 0xfffedcba", pixels[1] == 0xfffedcba);
        free(pdata_rgba_file.bmp_out);
    }

    /* -------------------------------------------------------------
     * 5. RGB PNG from Buffer (no alpha in stream, asserts FIX_ALPHA = 0xff)
     * ------------------------------------------------------------- */
    printf("\n--- Test 5: pngLoadFromBuffer (RGB - FIX_ALPHA) ---\n");
    pngData pdata_rgb_buf;
    memset(&pdata_rgb_buf, 0, sizeof(pdata_rgb_buf));
    int32_t pret_rgb = pngLoadFromBuffer(test_rgb_png, test_rgb_png_size, &pdata_rgb_buf);
    printf("  pngLoadFromBuffer (RGB) ret=0x%08x width=%u height=%u pitch=%u bmp=%p\n",
           (unsigned)pret_rgb, (unsigned)pdata_rgb_buf.width, (unsigned)pdata_rgb_buf.height,
           (unsigned)pdata_rgb_buf.pitch, pdata_rgb_buf.bmp_out);
    check("pngLoadFromBuffer (RGB) return code == 0", pret_rgb == 0);
    check("pngLoadFromBuffer (RGB) width == 8", pdata_rgb_buf.width == 8);
    check("pngLoadFromBuffer (RGB) height == 8", pdata_rgb_buf.height == 8);
    check("pngLoadFromBuffer (RGB) pitch == 32", pdata_rgb_buf.pitch == 32);
    check("pngLoadFromBuffer (RGB) bmp_out != NULL", pdata_rgb_buf.bmp_out != NULL);

    if (pdata_rgb_buf.bmp_out) {
        uint32_t *pixels = (uint32_t *)pdata_rgb_buf.bmp_out;
        printf("  pixel(0,0)=0x%08x, pixel(1,0)=0x%08x\n",
               (unsigned)pixels[0], (unsigned)pixels[1]);
        /* Exact ARGB words:
         * (0,0): RGB=(0x33, 0x77, 0xbb) -> FIX_ALPHA sets A=0xff -> ARGB = 0xff3377bb
         * (1,0): RGB=(0x99, 0x11, 0x55) -> FIX_ALPHA sets A=0xff -> ARGB = 0xff991155
         */
        check("pngLoadFromBuffer (RGB) pixel(0,0) exact ARGB 0xff3377bb (FIX_ALPHA)", pixels[0] == 0xff3377bb);
        check("pngLoadFromBuffer (RGB) pixel(1,0) exact ARGB 0xff991155 (FIX_ALPHA)", pixels[1] == 0xff991155);
        free(pdata_rgb_buf.bmp_out);
    }

    /* -------------------------------------------------------------
     * 6. RGB PNG from File (/app_home/data/test_rgb.png)
     * ------------------------------------------------------------- */
    printf("\n--- Test 6: pngLoadFromFile (RGB - FIX_ALPHA) ---\n");
    pngData pdata_rgb_file;
    memset(&pdata_rgb_file, 0, sizeof(pdata_rgb_file));
    int32_t pfret_rgb = pngLoadFromFile("/app_home/data/test_rgb.png", &pdata_rgb_file);
    printf("  pngLoadFromFile (RGB) ret=0x%08x width=%u height=%u pitch=%u bmp=%p\n",
           (unsigned)pfret_rgb, (unsigned)pdata_rgb_file.width, (unsigned)pdata_rgb_file.height,
           (unsigned)pdata_rgb_file.pitch, pdata_rgb_file.bmp_out);
    check("pngLoadFromFile (RGB) return code == 0", pfret_rgb == 0);
    check("pngLoadFromFile (RGB) width == 8", pdata_rgb_file.width == 8);
    check("pngLoadFromFile (RGB) height == 8", pdata_rgb_file.height == 8);
    check("pngLoadFromFile (RGB) pitch == 32", pdata_rgb_file.pitch == 32);
    check("pngLoadFromFile (RGB) bmp_out != NULL", pdata_rgb_file.bmp_out != NULL);

    if (pdata_rgb_file.bmp_out) {
        uint32_t *pixels = (uint32_t *)pdata_rgb_file.bmp_out;
        printf("  file pixel(0,0)=0x%08x, pixel(1,0)=0x%08x\n",
               (unsigned)pixels[0], (unsigned)pixels[1]);
        check("pngLoadFromFile (RGB) pixel(0,0) exact ARGB 0xff3377bb (FIX_ALPHA)", pixels[0] == 0xff3377bb);
        check("pngLoadFromFile (RGB) pixel(1,0) exact ARGB 0xff991155 (FIX_ALPHA)", pixels[1] == 0xff991155);
        free(pdata_rgb_file.bmp_out);
    }

    cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);
    cellSysmoduleUnloadModule(CELL_SYSMODULE_JPGDEC);

    printf("\nTotal failures: %d\n", failures);
    if (failures == 0) {
        printf("LEGACY_IMAGE_DEC_OK\n");
        return 0;
    } else {
        printf("LEGACY_IMAGE_DEC_FAIL\n");
        return 1;
    }
}
