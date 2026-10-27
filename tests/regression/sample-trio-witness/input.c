/*
 * hello-ppu-input-combined witness. Every pad, keyboard, mouse and
 * sysutil call the sample makes goes to a fake here.
 *
 *   case 0 absent:          no device on any port; must pass.
 *   case 1 idle:            all three connected, no new data; must pass.
 *   case 2 unplug-race:     mouse read fails and a second info query shows
 *                           it unplugged; must pass (normal observation).
 *   case 3 kb-init-fail:    cellKbInit fails; must fail without entering
 *                           the loop, still ending pad and mouse and
 *                           unregistering the callback.
 *   case 4 dispatch-fail:   cellSysutilCheckCallback fails on its third
 *                           call; must fail.
 *   case 5 read-fail:       mouse read fails while still connected; must
 *                           fail.
 *   case 6 end-fail:        cellPadEnd fails; must fail.
 */
#include <stdint.h>
#include <string.h>
#include <cell/pad.h>
#include <cell/keyboard.h>
#include <cell/mouse.h>
#include <cell/sysutil.h>

#include "witness.h"

#define FAKE_ERR ((int32_t)0x80121104u)

static int pad_on, kb_on, mouse_on;
static int n_register, n_unregister, n_check, n_pad_end, n_kb_end, n_mouse_end;
static int n_mouse_data, n_mouse_info;

static int fake_register(int slot, CellSysutilCallback func, void *userdata)
{
    (void)slot; (void)func; (void)userdata;
    n_register++;
    return CELL_OK;
}

static int fake_unregister(int slot)
{
    (void)slot;
    n_unregister++;
    return CELL_OK;
}

static int fake_check(void)
{
    n_check++;
    return (WITNESS_CASE == 4 && n_check == 3) ? (int)0x8002b102u : CELL_OK;
}

static int32_t fake_pad_init(uint32_t n) { (void)n; return CELL_PAD_OK; }
static int32_t fake_pad_end(void)
{
    n_pad_end++;
    return WITNESS_CASE == 6 ? FAKE_ERR : CELL_PAD_OK;
}
static int32_t fake_pad_info2(CellPadInfo2 *info)
{
    memset(info, 0, sizeof(*info));
    info->port_status[0] = pad_on ? CELL_PAD_STATUS_CONNECTED : 0;
    return CELL_PAD_OK;
}
static int32_t fake_pad_data(uint32_t port, CellPadData *d)
{
    (void)port;
    memset(d, 0, sizeof(*d));   /* len 0: no change */
    return CELL_PAD_OK;
}

static int32_t fake_kb_init(uint32_t n)
{
    (void)n;
    return WITNESS_CASE == 3 ? FAKE_ERR : CELL_OK;
}
static int32_t fake_kb_end(void) { n_kb_end++; return CELL_OK; }
static int32_t fake_kb_info(CellKbInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->status[0] = kb_on ? CELL_KB_STATUS_CONNECTED : 0;
    return CELL_OK;
}
static int32_t fake_kb_read(uint32_t port, CellKbData *d)
{
    (void)port;
    memset(d, 0, sizeof(*d));   /* no keycodes */
    return CELL_OK;
}

static int32_t fake_mouse_init(uint32_t n) { (void)n; return CELL_OK; }
static int32_t fake_mouse_end(void) { n_mouse_end++; return CELL_OK; }
static int32_t fake_mouse_info(CellMouseInfo *info)
{
    n_mouse_info++;
    memset(info, 0, sizeof(*info));
    info->status[0] = mouse_on ? CELL_MOUSE_STATUS_CONNECTED : 0;
    return CELL_OK;
}
static int32_t fake_mouse_data(uint32_t port, CellMouseData *d)
{
    (void)port;
    n_mouse_data++;
    memset(d, 0, sizeof(*d));   /* no update */
    if (WITNESS_CASE == 2 && n_mouse_data == 2) {
        mouse_on = 0;           /* unplugged between info and read */
        return FAKE_ERR;
    }
    if (WITNESS_CASE == 5 && n_mouse_data == 2)
        return FAKE_ERR;        /* still connected: a real failure */
    return CELL_OK;
}

#define cellSysutilRegisterCallback   fake_register
#define cellSysutilUnregisterCallback fake_unregister
#define cellSysutilCheckCallback      fake_check
#define cellPadInit     fake_pad_init
#define cellPadEnd      fake_pad_end
#define cellPadGetInfo2 fake_pad_info2
#define cellPadGetData  fake_pad_data
#define cellKbInit      fake_kb_init
#define cellKbEnd       fake_kb_end
#define cellKbGetInfo   fake_kb_info
#define cellKbRead      fake_kb_read
#define cellMouseInit    fake_mouse_init
#define cellMouseEnd     fake_mouse_end
#define cellMouseGetInfo fake_mouse_info
#define cellMouseGetData fake_mouse_data
#define main sample_main
#include "sysutil/hello-ppu-input-combined/source/main.c"
#undef main

int main(void)
{
    int on = WITNESS_CASE != 0;
    pad_on = kb_on = mouse_on = on;

    int rc = sample_main(0, NULL);
    printf("witness %s: sample exit status %d, checks=%d mouse info=%d reads=%d\n",
           WITNESS_NAME, rc, n_check, n_mouse_info, n_mouse_data);

    WITNESS_REQUIRE(n_register == 1);
    WITNESS_REQUIRE(n_unregister == 1);
    WITNESS_REQUIRE(n_pad_end == 1);
    WITNESS_REQUIRE(n_mouse_end == 1);
    switch (WITNESS_CASE) {
    case 0:
    case 1:
        WITNESS_REQUIRE(rc == 0);
        WITNESS_REQUIRE(n_check == MAX_FRAME);
        WITNESS_REQUIRE(n_kb_end == 1);
        break;
    case 2:
        WITNESS_REQUIRE(rc == 0);
        WITNESS_REQUIRE(n_check == MAX_FRAME);
        WITNESS_REQUIRE(n_mouse_data == 2);      /* no reads once unplugged */
        WITNESS_REQUIRE(n_kb_end == 1);
        break;
    case 3:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_check == 0);           /* loop never entered */
        WITNESS_REQUIRE(n_kb_end == 0);          /* kb was never set up */
        break;
    case 4:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_check == 3);           /* stopped at the failure */
        WITNESS_REQUIRE(n_kb_end == 1);
        break;
    case 5:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_mouse_data == 2);      /* stopped at the failure */
        WITNESS_REQUIRE(n_kb_end == 1);
        break;
    case 6:
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(n_check == MAX_FRAME);
        WITNESS_REQUIRE(n_kb_end == 1);
        break;
    default:
        WITNESS_REQUIRE(!"unknown case");
    }
    return witness_finish();
}
