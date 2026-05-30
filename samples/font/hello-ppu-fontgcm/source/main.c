#include <stdio.h>

#include <sys/process.h>
#include <cell/fontGcm.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    CellFontInitGraphicsConfigGcm config;
    const CellFontGraphics *graphics = 0;

    CellFontInitGraphicsConfigGcm_initialize(&config);
    int rc = cellFontInitGraphicsGcm(&config, &graphics);
    printf("cellFontInitGraphicsGcm -> 0x%08x graphics=%p\n",
           (unsigned)rc, (const void *)graphics);
    if (rc != CELL_OK) {
        return 1;
    }

    uint32_t packet_mode = 0;
    CellFontGraphicsDrawContext draw_context;
    rc = cellFontGraphicsSetGcmPacketMode(
        &draw_context, CELL_FONT_GRAPHICS_GCM_PACKET_WRITE_ENABLE);
    if (rc != CELL_OK) {
        return 1;
    }

    rc = cellFontGraphicsGetGcmPacketMode(&draw_context, &packet_mode);
    printf("cellFontGraphicsGetGcmPacketMode -> 0x%08x mode=0x%08x\n",
           (unsigned)rc, (unsigned)packet_mode);

    return rc == CELL_OK ? 0 : 1;
}
