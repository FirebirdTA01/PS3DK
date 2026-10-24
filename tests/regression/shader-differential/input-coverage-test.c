#include <stdint.h>
#include <stdio.h>
#include "source/input_coverage.h"

static int failures;
static void check(const char *name, uint32_t got, uint32_t expected)
{
    if (got != expected) {
        printf("FAIL %s: got %08x expected %08x\n", name,
               (unsigned)got, (unsigned)expected);
        failures++;
    }
}
int main(void)
{
    unsigned i;
    check("vp-tex9-register6", sd_vp_output_channel(6, 1u << 13), 1u << 10);
    check("vp-point-size-is-not-tex9", sd_vp_output_channel(6, 1u << 5), 0);
    check("vp-register16-is-not-tex9", sd_vp_output_channel(16, 1u << 13), 0);
    check("vp-tex8-register15", sd_vp_output_channel(15, 1u << 12), 1u << 9);
    for (i = 0; i < 8; ++i)
        check("vp-tex0-7", sd_vp_output_channel(7 + i, 1u << (14 + i)), 1u << (1 + i));
    /* Shared VP: HPOS, TEX0..3, COL0. Actual GB shader input contract
     * asks for TEX0..6: TEX4..6 must refuse before either draw. */
    const uint32_t shared = 1u | (15u << 1) | (1u << 11);
    check("shared-vp-missing-tex4-6", sd_missing_input_channels(0x1fc000, shared), 0xe0);
    check("extended-vp-supplies-tex4-6", sd_missing_input_channels(0x1fc000, 0x3fff), 0);
    check("no-varying-inputs", sd_missing_input_channels(0, 0), 0);
    for (i = 0; i < 10; ++i) {
        const uint32_t attr = 1u << (i < 8 ? 14 + i : 4 + i);
        const uint32_t channel = 1u << (i + 1);
        check("texture-missing", sd_missing_input_channels(attr, 0), channel);
        check("texture-present", sd_missing_input_channels(attr, channel), 0);
        check("other-texture-is-not-enough", sd_missing_input_channels(attr, channel ^ 0x3fff), channel);
    }
    check("front-colour", sd_missing_input_channels(1, 0), 1u << 11);
    check("back-colour", sd_missing_input_channels(4, 0), 1u << 11);
    check("colour-aliases", sd_missing_input_channels(5, 1u << 11), 0);
    check("secondary-colour", sd_missing_input_channels(10, 0), 1u << 12);
    check("fog", sd_missing_input_channels(16, 0), 1u << 13);
    check("unknown-attribute", sd_missing_input_channels(1u << 31, 0x3fff), 0x80000000u);
    printf("input-coverage-test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
