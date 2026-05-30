#include <stdint.h>
#include <stdio.h>

#include <sys/process.h>
#include <sysutil_avc2.h>
#include <sysutil_avc2_video.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static void avc2_callback(CellSysutilAvc2EventId event_id,
                          CellSysutilAvc2EventParam event_param,
                          void *userdata)
{
    (void)event_id;
    (void)event_param;
    (void)userdata;
}

int main(void)
{
    CellSysutilAvc2InitParam init_param;
    uint32_t container_size = 0;
    uint8_t visible = 0;

    int rc = cellSysutilAvc2InitParam(CELL_SYSUTIL_AVC2_INIT_PARAM_VERSION,
                                      &init_param);
    printf("cellSysutilAvc2InitParam -> 0x%08x\n", (unsigned)rc);

    init_param.media_type = CELL_SYSUTIL_AVC2_VOICE_CHAT;
    init_param.max_players = 2;
    init_param.voice_param.voice_quality = CELL_SYSUTIL_AVC2_VOICE_QUALITY_NORMAL;

    rc = cellSysutilAvc2EstimateMemoryContainerSize(&init_param,
                                                    &container_size);
    printf("cellSysutilAvc2EstimateMemoryContainerSize -> 0x%08x size=%u\n",
           (unsigned)rc, (unsigned)container_size);

    rc = cellSysutilAvc2GetScreenShowStatus(&visible);
    printf("cellSysutilAvc2GetScreenShowStatus -> 0x%08x visible=%u\n",
           (unsigned)rc, visible);

    (void)avc2_callback;
    return 0;
}
