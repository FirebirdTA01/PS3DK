/*
 * hello-ppu-input-combined - pad + keyboard + mouse polled in one event
 * loop, with hot-plug reporting on port 0 of each surface.
 *
 * Uses, against the SDK headers we ship:
 *   <cell/pad.h>      cellPadInit / GetInfo2 / GetData / End
 *   <cell/keyboard.h> cellKbInit / GetInfo / Read / End
 *   <cell/mouse.h>    cellMouseInit / GetInfo / GetData / End
 *   <cell/sysutil.h>  cellSysutilRegisterCallback / CheckCallback /
 *                     UnregisterCallback (for XMB quit)
 *
 * Behaviour on RPCS3 / PS3:
 *   1. Registers a sysutil callback and initialises all three surfaces.
 *      Any of these failing is an error: the loop is skipped, whatever
 *      was set up is shut down, and the run ends in FAIL.
 *   2. Every frame (8 ms) it dispatches sysutil callbacks first and stops
 *      at once on XMB quit, then reads port 0 of each surface once. A
 *      status block is printed every 60 frames. The loop ends after
 *      MAX_FRAME (600) frames, on XMB quit, or on pad START or SELECT.
 *   3. Hot-plug: the port 0 connection bit of each surface
 *      (CELL_PAD_STATUS_CONNECTED in port_status, CELL_KB_STATUS_CONNECTED
 *      and CELL_MOUSE_STATUS_CONNECTED in status[]) is compared with the
 *      previous frame and a change prints a CONNECTED / DISCONNECTED line.
 *
 * What counts as normal: no device on a port, a frame with no new data
 * (pad len 0, no keycodes, no mouse update), and a read that fails because
 * the device was unplugged between the info query and the read, when a
 * second info query confirms the port is now disconnected. Every other
 * failed call, including sysutil dispatch and the shutdown calls, is
 * counted as an error and printed at once; any error makes the last line
 * "hello-ppu-input-combined: FAIL (...)" and the exit status 1. Otherwise
 * it is "hello-ppu-input-combined: PASS (...)" and 0.
 *
 * A run with no devices attached exercises the calls but not hot-plug
 * itself; that needs devices plugged or unplugged while it runs.
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

SYS_PROCESS_PARAM(1001, 0x100000);

#ifndef MAX_FRAME
#define MAX_FRAME    600
#endif
#define SLEEP_US     (8 * 1000)
#define MAX_DEVS     7
#define REPORT_EVERY 60   /* print a status block every N frames */

static volatile int g_exit_requested;
static int g_errors;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
    (void)param; (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME) {
        printf("  event: EXIT_GAME\n");
        g_exit_requested = 1;
    }
}

static void count_error(const char *what, int32_t rc)
{
    g_errors++;
    printf("  ERROR %s -> 0x%08x\n", what, (unsigned)rc);
}

/* -1 = not read yet; otherwise the port 0 connection bit last seen. */
static int prev_pad = -1, prev_kb = -1, prev_mouse = -1;

static void note_connection(const char *surface, int *prev, int now)
{
    if (*prev >= 0 && now != *prev)
        printf("    HOT-PLUG %s port 0: %s\n", surface,
               now ? "CONNECTED" : "DISCONNECTED");
    *prev = now;
}

/* ---- pad --------------------------------------------------------- */

static int pad_port0(void)
{
    CellPadInfo2 info;
    memset(&info, 0, sizeof(info));
    int32_t rc = cellPadGetInfo2(&info);
    if (rc != CELL_PAD_OK) {
        count_error("cellPadGetInfo2", rc);
        return -1;
    }
    return (info.port_status[0] & CELL_PAD_STATUS_CONNECTED) != 0;
}

static void poll_pad(int report)
{
    int now = pad_port0();
    if (now < 0)
        return;
    note_connection("pad", &prev_pad, now);
    if (!now) {
        if (report) printf("    pad:   port 0 not connected\n");
        return;
    }

    CellPadData d;
    memset(&d, 0, sizeof(d));
    int32_t rc = cellPadGetData(0, &d);
    if (rc != CELL_PAD_OK) {
        int again = pad_port0();
        if (again == 0) {
            printf("    pad:   unplugged during read (0x%08x)\n", (unsigned)rc);
            note_connection("pad", &prev_pad, 0);
        } else if (again > 0) {
            count_error("cellPadGetData(0)", rc);
        }
        return;
    }
    if (d.len == 0) {
        if (report) printf("    pad:   idle (no change this frame)\n");
        return;
    }
    if (d.len <= CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) {
        /* Too short to hold the fields read below. */
        if (report) printf("    pad:   short report (len=%d) not used\n", (int)d.len);
        return;
    }
    uint32_t d1 = d.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
    uint32_t lx = d.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
    uint32_t rx = d.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
    if (report)
        printf("    pad:   d1=0x%02lx  L-x=%3lu  R-x=%3lu\n",
               (unsigned long)d1, (unsigned long)lx, (unsigned long)rx);
    if (d1 & (CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_START)) {
        printf("  pad START or SELECT: stopping\n");
        g_exit_requested = 1;
    }
}

/* ---- keyboard ---------------------------------------------------- */

static int kb_port0(void)
{
    CellKbInfo info;
    memset(&info, 0, sizeof(info));
    int32_t rc = cellKbGetInfo(&info);
    if (rc != CELL_OK) {
        count_error("cellKbGetInfo", rc);
        return -1;
    }
    return (info.status[0] & CELL_KB_STATUS_CONNECTED) != 0;
}

static void poll_kb(int report)
{
    int now = kb_port0();
    if (now < 0)
        return;
    note_connection("keyboard", &prev_kb, now);
    if (!now) {
        if (report) printf("    kb:    port 0 not connected\n");
        return;
    }

    CellKbData d;
    memset(&d, 0, sizeof(d));
    int32_t rc = cellKbRead(0, &d);
    if (rc != CELL_OK) {
        int again = kb_port0();
        if (again == 0) {
            printf("    kb:    unplugged during read (0x%08x)\n", (unsigned)rc);
            note_connection("keyboard", &prev_kb, 0);
        } else if (again > 0) {
            count_error("cellKbRead(0)", rc);
        }
        return;
    }
    if (d.len <= 0) {
        if (report) printf("    kb:    idle (no keycodes this frame)\n");
        return;
    }
    if (report) {
        printf("    kb:    %d key(s):", (int)d.len);
        for (int k = 0; k < d.len && k < 6; k++)
            printf(" 0x%04x", (unsigned)d.keycode[k]);
        printf("\n");
    }
}

/* ---- mouse ------------------------------------------------------- */

static int mouse_port0(void)
{
    CellMouseInfo info;
    memset(&info, 0, sizeof(info));
    int32_t rc = cellMouseGetInfo(&info);
    if (rc != CELL_OK) {
        count_error("cellMouseGetInfo", rc);
        return -1;
    }
    return (info.status[0] & CELL_MOUSE_STATUS_CONNECTED) != 0;
}

static void poll_mouse(int report)
{
    int now = mouse_port0();
    if (now < 0)
        return;
    note_connection("mouse", &prev_mouse, now);
    if (!now) {
        if (report) printf("    mouse: port 0 not connected\n");
        return;
    }

    CellMouseData d;
    memset(&d, 0, sizeof(d));
    int32_t rc = cellMouseGetData(0, &d);
    if (rc != CELL_OK) {
        int again = mouse_port0();
        if (again == 0) {
            printf("    mouse: unplugged during read (0x%08x)\n", (unsigned)rc);
            note_connection("mouse", &prev_mouse, 0);
        } else if (again > 0) {
            count_error("cellMouseGetData(0)", rc);
        }
        return;
    }
    if (d.update != CELL_MOUSE_DATA_UPDATE) {
        if (report) printf("    mouse: idle (no movement this frame)\n");
        return;
    }
    if (report)
        printf("    mouse: dx=%d dy=%d wheel=%d buttons=0x%02x\n",
               (int)d.x_axis, (int)d.y_axis, (int)d.wheel, (unsigned)d.buttons);
}

/* ---- main -------------------------------------------------------- */

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;

    printf("hello-ppu-input-combined: pad + keyboard + mouse with hot-plug\n");

    int32_t rc = cellSysutilRegisterCallback(0, on_sysutil_event, NULL);
    int cb_registered = (rc == CELL_OK);
    if (!cb_registered)
        count_error("cellSysutilRegisterCallback(0)", rc);

    rc = cellPadInit(MAX_DEVS);
    int pad_ok = (rc == CELL_PAD_OK);
    if (!pad_ok) count_error("cellPadInit", rc);
    rc = cellKbInit(MAX_DEVS);
    int kb_ok = (rc == CELL_OK);
    if (!kb_ok) count_error("cellKbInit", rc);
    rc = cellMouseInit(MAX_DEVS);
    int mouse_ok = (rc == CELL_OK);
    if (!mouse_ok) count_error("cellMouseInit", rc);
    printf("  init: callback=%d pad=%d kb=%d mouse=%d\n",
           cb_registered, pad_ok, kb_ok, mouse_ok);

    int frames = 0;
    const char *why = "setup failed";
    if (g_errors == 0) {
        why = "all frames";
        for (; frames < MAX_FRAME; frames++) {
            rc = cellSysutilCheckCallback();
            if (rc != CELL_OK) {
                count_error("cellSysutilCheckCallback", rc);
                why = "error";
                break;
            }
            if (g_exit_requested) {
                why = "exit requested";
                break;
            }

            int report = (frames % REPORT_EVERY) == 0;
            if (report) printf("frame %3d:\n", frames);
            poll_pad(report);
            poll_kb(report);
            poll_mouse(report);
            if (g_errors) {
                why = "error";
                break;
            }
            if (g_exit_requested) {
                why = "exit requested";
                frames++;
                break;
            }
            usleep(SLEEP_US);
        }
    }

    if (mouse_ok && (rc = cellMouseEnd()) != CELL_OK) count_error("cellMouseEnd", rc);
    if (kb_ok    && (rc = cellKbEnd())    != CELL_OK) count_error("cellKbEnd", rc);
    if (pad_ok   && (rc = cellPadEnd())   != CELL_PAD_OK) count_error("cellPadEnd", rc);
    if (cb_registered && (rc = cellSysutilUnregisterCallback(0)) != CELL_OK)
        count_error("cellSysutilUnregisterCallback(0)", rc);

    printf("hello-ppu-input-combined: %s (frames=%d, %s, errors=%d)\n",
           g_errors ? "FAIL" : "PASS", frames, why, g_errors);
    return g_errors ? 1 : 0;
}
