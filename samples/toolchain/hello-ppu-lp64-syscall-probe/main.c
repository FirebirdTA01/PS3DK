#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <sys/sys_time.h>
#include <cell/gcm.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>

int main(void) {
    printf("probe: LP64 cellgcm-clear bisect — rsxMemalign test\n");

    void *host_addr = memalign(1024 * 1024, 32 * 1024 * 1024);
    printf("probe: cellGcmInit...\n");
    cellGcmInit(0x100000, 32 * 1024 * 1024, host_addr);

    videoState state;
    printf("probe: videoGetState...\n");
    videoGetState(0, 0, &state);

    videoResolution res;
    printf("probe: videoGetResolution...\n");
    videoGetResolution(state.displayMode.resolution, &res);

    videoConfiguration vcfg;
    memset(&vcfg, 0, sizeof(vcfg));
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch      = res.width * sizeof(uint32_t);
    vcfg.aspect     = state.displayMode.aspect;
    printf("probe: videoConfigure...\n");
    videoConfigure(0, &vcfg, NULL, 0);

    cellGcmSetFlipMode(GCM_FLIP_VSYNC);

    printf("probe: rsxMemalign(64, %u)...\n", res.height * res.width * 8);
    uint32_t depth_pitch = res.width * sizeof(uint32_t);
    void *depth_buffer = rsxMemalign(64, res.height * depth_pitch * 2);
    printf("probe: rsxMemalign returned %p\n", depth_buffer);

    u32 depth_offset;
    cellGcmAddressToOffset(depth_buffer, &depth_offset);
    printf("probe: cellGcmAddressToOffset -> off=0x%x\n", depth_offset);

    printf("probe: calling sys_time_get_timebase_frequency()\n");
    uint64_t freq = sys_time_get_timebase_frequency();
    printf("probe: freq = 0x%lx\n", (unsigned long)freq);
    printf("probe: done\n");
    return 0;
}
