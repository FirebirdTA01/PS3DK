#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

int eye_yuv422_rgba(const uint8_t *, size_t, size_t, uint8_t *, size_t, size_t, unsigned, unsigned);
int eye_rgba_xrgb(const uint8_t *, size_t, size_t, uint8_t *, size_t, size_t, unsigned, unsigned);
static unsigned passed, failed;
#define CHECK(name, expr) do { if (expr) { ++passed; } else { ++failed; printf("FAIL %s line %d\n", name, __LINE__); } } while (0)

int main(void)
{
    uint8_t source[32] = {0}, output[48], before[48];
    const uint8_t neutral[] = {16,128,235,128};
    const uint8_t black_white[] = {0,0,0,255,255,255,255,255};
    const uint8_t red_pair[] = {81,90,145,240};
    const uint8_t red_expected[] = {255,0,0,255,255,74,74,255};
    const uint8_t blue_pair[] = {41,240,41,110};
    const uint8_t blue_expected[] = {0,0,255,255,0,0,255,255};
    memset(output, 0x5a, sizeof(output));
    memcpy(source + 1, neutral, 4);
    CHECK("byte-aligned black/white", eye_yuv422_rgba(source+1,4,4,output+1,8,8,2,1)==0 && !memcmp(output+1,black_white,8));
    CHECK("unaligned destination guards", output[0]==0x5a && output[9]==0x5a);
    CHECK("asymmetric chroma and luma", eye_yuv422_rgba(red_pair,4,4,output,8,8,2,1)==0 && !memcmp(output,red_expected,8));
    CHECK("blue and clipping", eye_yuv422_rgba(blue_pair,4,4,output,8,8,2,1)==0 && !memcmp(output,blue_expected,8));
    source[1]=0; source[2]=128; source[3]=255; source[4]=128;
    CHECK("limited range clipping", eye_yuv422_rgba(source+1,4,4,output,8,8,2,1)==0 && !memcmp(output,black_white,8));
    memcpy(source+1,neutral,4); memcpy(source+8,red_pair,4);
    memset(output,0x5a,sizeof(output));
    CHECK("padded source and output", eye_yuv422_rgba(source+1,11,7,output+1,19,11,2,2)==0 && !memcmp(output+1,black_white,8) && !memcmp(output+12,red_expected,8));
    CHECK("padding untouched", output[9]==0x5a && output[10]==0x5a && output[11]==0x5a && output[20]==0x5a);
    memcpy(before,output,sizeof(output));
    CHECK("short input", eye_yuv422_rgba(source,3,4,output,8,8,2,1)==-1);
    CHECK("short output", eye_yuv422_rgba(source,4,4,output,7,8,2,1)==-1);
    CHECK("odd width", eye_yuv422_rgba(source,8,8,output,12,12,3,1)==-1);
    CHECK("zero width", eye_yuv422_rgba(source,4,4,output,8,8,0,1)==-1);
    CHECK("zero height", eye_yuv422_rgba(source,4,4,output,8,8,2,0)==-1);
    CHECK("short input pitch", eye_yuv422_rgba(source,8,3,output,16,8,2,2)==-1);
    CHECK("short output pitch", eye_yuv422_rgba(source,8,4,output,16,7,2,2)==-1);
    CHECK("input span overflow", eye_yuv422_rgba(source,SIZE_MAX,SIZE_MAX,output,24,8,2,3)==-1);
    CHECK("output span overflow", eye_yuv422_rgba(source,12,4,output,SIZE_MAX,SIZE_MAX,2,3)==-1);
    CHECK("null input", eye_yuv422_rgba(NULL,4,4,output,8,8,2,1)==-1);
    CHECK("null output", eye_yuv422_rgba(source,4,4,NULL,8,8,2,1)==-1);
    CHECK("rejected rows never write", !memcmp(output,before,sizeof(output)));
    CHECK("overlap refused", eye_yuv422_rgba(source,4,4,source+1,8,8,2,1)==-1);
    {
        const uint8_t rgba[] = {19,31,47,255,53,79,97,255};
        const uint8_t xrgb[] = {255,19,31,47,255,53,79,97};
        memset(output,0x5a,sizeof(output));
        CHECK("XRGB byte order", eye_rgba_xrgb(rgba,8,8,output+1,8,8,2,1)==0 && !memcmp(output+1,xrgb,8));
        CHECK("XRGB guards", output[0]==0x5a && output[9]==0x5a);
        CHECK("XRGB short buffer", eye_rgba_xrgb(rgba,7,8,output,8,8,2,1)==-1);
    }
    printf("eye-camera: %u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
