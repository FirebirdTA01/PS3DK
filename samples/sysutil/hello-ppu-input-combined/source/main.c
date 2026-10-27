/*
 * hello-ppu-input-combined - pad + keyboard + mouse polled in one event
 * loop, with hot-plug detection on all three surfaces.
 *
 * Validates, against the SDK headers we ship:
 *   <cell/pad.h>      cellPadInit / GetInfo2 / GetData / End
 *   <cell/keyboard.h> cellKbInit / GetInfo / Read / End
 *   <cell/mouse.h>    cellMouseInit / GetInfo / GetData / End
 *   <cell/sysutil.h>  cellSysutilRegisterCallback (for XMB Quit)
 *
 * Behaviour on RPCS3 / PS3:
 *   1. Inits all three surfaces; reports how many devices each reports
 *      connected (pad per port via port_status; kb/mouse via their
 *      "now connected" count field).
 *   2. Polls port 0 of each surface every frame and prints one line per
 *      surface (or "not connected" / "idle"); the sample exits after 600
 *      frames, on XMB Quit, or on SELECT or START (pad).
 *   3. Hot-plug: each frame the previous connection state is diffed. A
 *      pad port whose CELL_PAD_STATUS_CONNECTED bit toggles, or a
 *      keyboard/mouse whose connected-count changes, is printed as a
 *      CONNECTED / DISCONNECTED transition. On real hardware this is
 *      observable by plugging/unplugging a USB pad, keyboard or mouse
 *      while the sample is running.
 *
 * The sample is green with no devices present: RPCS3's input HLE
 * delivers zero-keycode frames, so an absent device is not a failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>

#include <cell/pad.h>
#include <cell/keyboard.h>
#include <cell/mouse.h>
#include <cell/sysutil.h>

SYS_PROCESS_PARAM(2048, 0x100000);

#define MAX_FRAME  600
#define SLEEP_US   (8 * 1000)
#define MAX_DEVS   7

static volatile int g_exit_requested = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
    (void)param; (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME) {
        printf("  event: EXIT_GAME\n");
        g_exit_requested = 1;
    }
}

/* --- per-surface "port 0 connected right now?" probes -------------- */

static int pad_port0_connected(const CellPadInfo2 *i)
{
    return (i->port_status[0] & CELL_PAD_STATUS_CONNECTED) != 0;
}

static int mouse_now_connected(const CellMouseInfo *i)
{
    return (i->connected != 0);
}

/* --- per-surface one-line read ------------------------------------- */

static void print_pad(const CellPadInfo2 *info)
{
    if (!pad_port0_connected(info)) {
        printf("    pad:   port 0 not connected\n");
        return;
    }
    CellPadData d; memset(&d, 0, sizeof(d));
    if (cellPadGetData(0, &d) != CELL_PAD_OK) {
        printf("    pad:   GetData failed\n");
        return;
    }
    uint32_t d1 = d.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
    uint32_t lx = d.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
    uint32_t rx = d.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
    printf("    pad:   d1=0x%02lx  L-x=%3lu  R-x=%3lu\n",
           (unsigned long)d1, (unsigned long)lx, (unsigned long)rx);
    if (d1 & CELL_PAD_CTRL_SELECT) g_exit_requested = 1;
    if (d1 & CELL_PAD_CTRL_START)  g_exit_requested = 1;
}

static void print_kb(void)
{
    CellKbInfo i; memset(&i, 0, sizeof(i));
    if (cellKbGetInfo(&i) != CELL_OK) {
        printf("    kb:    GetInfo failed\n");
        return;
    }
    if (i.now_connect == 0) {
        printf("    kb:    no keyboard connected\n");
        return;
    }
    CellKbData d; memset(&d, 0, sizeof(d));
    if (cellKbRead(0, &d) != CELL_OK) {
        printf("    kb:    Read failed\n");
        return;
    }
    if (d.len <= 0) {
        printf("    kb:    idle (no keycodes this frame)\n");
        return;
    }
    printf("    kb:    %d key(s):", d.len);
    for (int k = 0; k < d.len && k < 6; k++)
        printf(" 0x%04x", (unsigned)d.keycode[k]);
    printf("\n");
}

static void print_mouse(void)
{
    CellMouseInfo i; memset(&i, 0, sizeof(i));
    if (cellMouseGetInfo(&i) != CELL_OK) {
        printf("    mouse: GetInfo failed\n");
        return;
    }
    if (!mouse_now_connected(&i)) {
        printf("    mouse: not connected\n");
        return;
    }
    CellMouseData d; memset(&d, 0, sizeof(d));
    if (cellMouseGetData(0, &d) != CELL_OK) {
        printf("    mouse: GetData failed\n");
        return;
    }
    if (d.update != CELL_MOUSE_DATA_UPDATE) {
        printf("    mouse: idle (no movement this frame)\n");
        return;
    }
    printf("    mouse: dx=%d dy=%d wheel=%d buttons=0x%02x\n",
           (int)d.x_axis, (int)d.y_axis, (int)d.wheel, (unsigned)d.buttons);
}

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;

    printf("hello-ppu-input-combined: pad + keyboard + mouse with hot-plug\n");

    int rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
    if (rc != CELL_OK)
        printf("  (sysutil callback register failed: 0x%08x; XMB Quit disabled)\n", (unsigned)rc);

    int pad_ok   = (cellPadInit(MAX_DEVS)   == CELL_PAD_OK);
    int kb_ok    = (cellKbInit(MAX_DEVS)    == CELL_OK);
    int mouse_ok = (cellMouseInit(MAX_DEVS) == CELL_OK);
    printf("  init: pad=%d  kb=%d  mouse=%d\n", (int)pad_ok, (int)kb_ok, (int)mouse_ok);
    if (!pad_ok && !kb_ok && !mouse_ok) {
        printf("  no input surface initialised; exiting\n");
        return 1;
    }

    /* previous connection states, for hot-plug diffing. */
    int prev_pad   = -1;   /* -1 = uninitialised */
    int prev_kb    = -1;
    int prev_mouse = -1;

    for (int frame = 0; frame < MAX_FRAME; frame++) {
        printf("frame %3d:\n", frame);

        if (pad_ok) {
            CellPadInfo2 info; memset(&info, 0, sizeof(info));
            if (cellPadGetInfo2(&info) == CELL_PAD_OK) {
                int now = pad_port0_connected(&info);
                if (prev_pad >= 0 && now != prev_pad)
                    printf("    HOT-PLUG pad port 0: %s\n",
                           now ? "CONNECTED" : "DISCONNECTED");
                prev_pad = now;
                print_pad(&info);
            } else {
                printf("    pad:   GetInfo2 failed\n");
            }
        }

        if (kb_ok) {
            CellKbInfo info; memset(&info, 0, sizeof(info));
            if (cellKbGetInfo(&info) == CELL_OK) {
                int now = (info.now_connect != 0);
                if (prev_kb >= 0 && now != prev_kb)
                    printf("    HOT-PLUG keyboard: %s (connected=%u)\n",
                           now ? "CONNECTED" : "DISCONNECTED",
                           (unsigned)info.now_connect);
                prev_kb = now;
                print_kb();
            } else {
                printf("    kb:    GetInfo failed\n");
            }
        }

        if (mouse_ok) {
            CellMouseInfo info; memset(&info, 0, sizeof(info));
            if (cellMouseGetInfo(&info) == CELL_OK) {
                int now = mouse_now_connected(&info);
                if (prev_mouse >= 0 && now != prev_mouse)
                    printf("    HOT-PLUG mouse: %s (connected=%u)\n",
                           now ? "CONNECTED" : "DISCONNECTED",
                           (unsigned)info.connected);
                prev_mouse = now;
                print_mouse();
            } else {
                printf("    mouse: GetInfo failed\n");
            }
        }

        if (g_exit_requested) break;
        usleep(SLEEP_US);
    }

    if (pad_ok)   cellPadEnd();
    if (kb_ok)    cellKbEnd();
    if (mouse_ok) cellMouseEnd();

    printf("hello-ppu-input-combined: done (%s)\n",
           g_exit_requested ? "early exit" : "all 600 frames");
    return 0;
}
