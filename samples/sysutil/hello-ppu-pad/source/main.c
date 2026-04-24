/*
 * hello-ppu-pad - cellPad API smoke test via nidgen stub archive.
 *
 * Validates the libio.a pipeline end-to-end: the nidgen archive built
 * from tools/nidgen/nids/extracted/libio_stub.yaml resolves every
 * cellPad* export the sample calls, bound against the sys_io SPRX at
 * runtime.
 *
 * Behaviour on RPCS3:
 *   1. cellPadInit(7).
 *   2. Poll port 0 for ~15 seconds, printing a snapshot each frame that
 *      any button / analog axis changes.
 *   3. If port 0 reports CELL_PAD_CAPABILITY_ACTUATOR, fire both motors
 *      at full power for ~250 ms once, then stop.
 *   4. Exit on SELECT + START, on cellSysutilCheckCallback exit event,
 *      or on poll-budget timeout.
 *
 * Sysutil callback runs alongside the pad loop so the PS button still
 * opens the XMB and a Quit from there exits the game cleanly.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>

#include <cell/pad.h>
#include <cell/sysutil.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile int g_exit_requested = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
    (void)param;
    (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME) {
        printf("  event: EXIT_GAME\n");
        g_exit_requested = 1;
    }
}

static void dump_port(uint32_t port, const CellPadInfo2 *info)
{
    const uint32_t status = info->port_status[port];
    const uint32_t caps   = info->device_capability[port];
    const uint32_t type   = info->device_type[port];
    printf("  port %u: status=0x%08x caps=0x%08x type=%u",
           (unsigned)port, (unsigned)status, (unsigned)caps, (unsigned)type);
    if (caps & CELL_PAD_CAPABILITY_PS3_CONFORMITY)   printf(" ps3");
    if (caps & CELL_PAD_CAPABILITY_PRESS_MODE)       printf(" press");
    if (caps & CELL_PAD_CAPABILITY_SENSOR_MODE)      printf(" sensor");
    if (caps & CELL_PAD_CAPABILITY_HP_ANALOG_STICK)  printf(" hp-analog");
    if (caps & CELL_PAD_CAPABILITY_ACTUATOR)         printf(" actuator");
    printf("\n");
}

static void dump_data(const CellPadData *d)
{
    const uint32_t d1 = d->button[CELL_PAD_BTN_OFFSET_DIGITAL1];
    const uint32_t d2 = d->button[CELL_PAD_BTN_OFFSET_DIGITAL2];
    const uint32_t lx = d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
    const uint32_t ly = d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
    const uint32_t rx = d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
    const uint32_t ry = d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y];
    printf("    btn d1=%02x d2=%02x  L=(%3u,%3u) R=(%3u,%3u)",
           (unsigned)d1, (unsigned)d2,
           (unsigned)lx, (unsigned)ly, (unsigned)rx, (unsigned)ry);
    if (d1 & CELL_PAD_CTRL_SELECT) printf(" SELECT");
    if (d1 & CELL_PAD_CTRL_START)  printf(" START");
    if (d1 & CELL_PAD_CTRL_UP)     printf(" UP");
    if (d1 & CELL_PAD_CTRL_DOWN)   printf(" DOWN");
    if (d1 & CELL_PAD_CTRL_LEFT)   printf(" LEFT");
    if (d1 & CELL_PAD_CTRL_RIGHT)  printf(" RIGHT");
    if (d2 & CELL_PAD_CTRL_CROSS)  printf(" X");
    if (d2 & CELL_PAD_CTRL_CIRCLE) printf(" O");
    if (d2 & CELL_PAD_CTRL_SQUARE) printf(" []");
    if (d2 & CELL_PAD_CTRL_TRIANGLE) printf(" /\\");
    printf("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("hello-ppu-pad: cellPad via nidgen libio stub\n");

    int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
    if (rc != 0) {
        printf("  cellSysutilRegisterCallback=0x%08x\n", (unsigned)rc);
        return 1;
    }

    rc = cellPadInit(CELL_PAD_MAX_PORT_NUM);
    if (rc != 0) {
        printf("  cellPadInit=0x%08x\n", (unsigned)rc);
        cellSysutilUnregisterCallback(0);
        return 1;
    }
    printf("  cellPadInit ok\n");

    CellPadInfo2 info;
    int rumble_fired = 0;
    int last_d1 = -1, last_d2 = -1;
    int last_connect_mask = 0;

    /* 30-second budget, 50ms tick.  RPCS3 can take a few ticks to
     * enumerate pads, so GetInfo2 gets re-queried every frame. */
    for (int tick = 0; tick < 600 && !g_exit_requested; tick++) {
        cellSysutilCheckCallback();

        memset(&info, 0, sizeof(info));
        rc = cellPadGetInfo2(&info);
        if (rc != 0) {
            if (tick % 20 == 0) {
                printf("  cellPadGetInfo2=0x%08x\n", (unsigned)rc);
            }
            usleep(50 * 1000);
            continue;
        }

        int connect_mask = 0;
        for (uint32_t p = 0; p < CELL_PAD_MAX_PORT_NUM; p++) {
            if (info.port_status[p] & CELL_PAD_STATUS_CONNECTED) {
                connect_mask |= (1u << p);
            }
        }
        if (connect_mask != last_connect_mask) {
            printf("  ports connected: now_connect=%u mask=0x%02x\n",
                   (unsigned)info.now_connect, (unsigned)connect_mask);
            for (uint32_t p = 0; p < CELL_PAD_MAX_PORT_NUM; p++) {
                if ((connect_mask & ~last_connect_mask) & (1u << p)) {
                    dump_port(p, &info);
                }
            }
            last_connect_mask = connect_mask;
        }

        if (!(connect_mask & 1u)) {
            usleep(50 * 1000);
            continue;
        }

        CellPadData d;
        memset(&d, 0, sizeof(d));
        rc = cellPadGetData(0, &d);
        if (rc == CELL_PAD_ERROR_NO_DEVICE) {
            usleep(50 * 1000);
            continue;
        }
        if (rc != 0) {
            printf("  cellPadGetData=0x%08x (tick=%d)\n", (unsigned)rc, tick);
            usleep(50 * 1000);
            continue;
        }

        if (d.len > 0) {
            const int d1 = d.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
            const int d2 = d.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
            if (d1 != last_d1 || d2 != last_d2) {
                dump_data(&d);
                last_d1 = d1;
                last_d2 = d2;
            }
            if ((d1 & CELL_PAD_CTRL_SELECT) && (d1 & CELL_PAD_CTRL_START)) {
                printf("  SELECT+START pressed -> exit\n");
                break;
            }
        }

        if (!rumble_fired && tick >= 60 &&
            (info.device_capability[0] & CELL_PAD_CAPABILITY_ACTUATOR)) {
            CellPadActParam ap;
            memset(&ap, 0, sizeof(ap));
            ap.motor[0] = 1;
            ap.motor[1] = 255;
            printf("  firing actuator (small=on, large=255)\n");
            (void)cellPadSetActDirect(0, &ap);
            usleep(250 * 1000);
            ap.motor[0] = 0;
            ap.motor[1] = 0;
            (void)cellPadSetActDirect(0, &ap);
            rumble_fired = 1;
        }

        usleep(50 * 1000);
    }

    cellPadEnd();
    cellSysutilUnregisterCallback(0);
    printf("result: %s\n", g_exit_requested ? "EXIT_GAME" : "timeout");
    return 0;
}
