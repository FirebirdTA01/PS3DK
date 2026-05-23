#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <cell/cell_video_out.h>
#include <cell/gcm.h>
#include <sysutil/video.h>

/* Bisect steps.  Build with -DSTEP=N, N=1..4 */
#ifndef STEP
#define STEP 1
#endif

int main(void) {
    printf("probe: LP64 cellGcm bisect step %d\n", STEP);

#if STEP >= 2
    void *host_addr = memalign(1024 * 1024, 32 * 1024 * 1024);
    printf("probe: cellGcmInit...\n");
    int r_init = cellGcmInit(0x100000, 32 * 1024 * 1024, host_addr);
    printf("probe: cellGcmInit returned %d\n", r_init);
#endif

#if STEP >= 3
    {
        CellGcmConfig cfg;
        cellGcmGetConfiguration(&cfg);
        u32 off;
        printf("probe: cellGcmAddressToOffset...\n");
        int r_ato = cellGcmAddressToOffset(cfg.localAddress, &off);
        printf("probe: cellGcmAddressToOffset returned %d, off=0x%x\n", r_ato, off);
    }
#endif

#if STEP >= 4
    {
        printf("probe: videoGetState...\n");
        videoState state;
        int r_vs = videoGetState(0, 0, &state);
        printf("probe: videoGetState returned %d, state=%d\n", r_vs, state.state);
    }
#endif

    printf("probe: cellVideoOutGetResolution(2, &res)...\n");
    CellVideoOutResolution res;
    int r = cellVideoOutGetResolution(2, &res);
    printf("probe: cellVideoOutGetResolution returned %d\n", r);

#if STEP >= 4
    if (r == 0) {
        printf("probe: res = %ux%u\n", res.width, res.height);
    }
#endif

    printf("probe: done\n");
    return 0;
}
