#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef BASELINE
static int sample_rgb_matches(uint32_t actual, uint32_t expected, unsigned tolerance)
{ (void)actual; (void)expected; (void)tolerance; return 1; }
#else
#include "../../samples/gcm/common/render_pixels.h"
#endif
int main(void)
{
    unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) ++failures; } while (0)
    CHECK(sample_rgb_matches(0x00ff0000,0xffff0000,0));
    CHECK(!sample_rgb_matches(0xff202830,0xffff0000,1));
    CHECK(!sample_rgb_matches(0xff0000ff,0xffff0000,1));
    CHECK(!sample_rgb_matches(0xff00ff00,0xffff0000,1));
    CHECK(sample_rgb_matches(0xff122232,0xff102030,2));
    CHECK(!sample_rgb_matches(0xff132232,0xff102030,2));
    CHECK(sample_rgb_matches(0xff000000,0xffffffff,255));
    CHECK(!sample_rgb_matches(0xff000000,0xffffffff,254));
#ifndef BASELINE
    uint32_t image[12*10], expected[4]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
    memset(image,0,sizeof image);
    /* Nonzero origin, padded pitch and integer magnification. */
    image[4*12+3]=expected[0]; image[4*12+5]=expected[1];
    image[6*12+3]=expected[2]; image[6*12+5]=expected[3];
#define PANEL() sample_check_panel(image,10,10,12,2,3,2,2,2,expected,0)
    CHECK(PANEL()==0);
    image[4*12+3]=0; CHECK(PANEL()==1);
    image[4*12+3]=expected[1]; image[4*12+5]=expected[0]; CHECK(PANEL()==2);
    memset(image,0,sizeof image); CHECK(PANEL()==4);
    CHECK(sample_check_panel(image,10,10,9,2,3,2,2,2,expected,0)==-1);
    CHECK(sample_check_panel(image,10,10,12,9,3,2,2,2,expected,0)==-1);
    CHECK(sample_check_panel(image,10,10,12,2,3,0,2,2,expected,0)==-1);
    CHECK(sample_check_panel(image,10,10,12,2,3,2,0,2,expected,0)==-1);
    CHECK(sample_blend_rgb(0x00ffffff,0xff202830)==0x202830);
    CHECK(sample_blend_rgb(0xffffffff,0xff202830)==0xffffff);
    CHECK(sample_blend_rgb(0x80ff0000,0xff000000)==0x800000);
#endif
    printf("pixel check: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
