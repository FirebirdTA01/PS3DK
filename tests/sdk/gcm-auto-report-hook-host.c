/* With PS3TC_PROF_AUTO the inline public report writers (cellGcmSetReport, cellGcmSetTimeStamp) call the AUTO
 * resource-write hook once per write (kind 1, the report index) before emitting their words; without it they
 * reference no hook at all. The emitted words are those of the natives either way. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cell/gcm.h>

#ifdef PS3TC_PROF_AUTO
static unsigned calls, kinds[4], indices[4];
#ifdef __cplusplus
extern "C"
#endif
void ps3dk_prof__auto_resource_write(struct _gcmCtxData *ctx, uint32_t kind, uint32_t index)
{
    (void)ctx;
    if (calls < 4) { kinds[calls] = kind; indices[calls] = index; }
    calls++;
}
#endif

static void fail(const char *what) { puts(what); exit(1); }

int main(void)
{
    static uint32_t a[16], b[16];
    CellGcmContextData n, r;
    memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
    n.begin = n.current = a; n.end = a + 16;
    r.begin = r.current = b; r.end = b + 16;
    cellGcmSetReport(&n, CELL_GCM_ZPASS_PIXEL_CNT, 600);
    ps3tc_gcm_state_report(&r, CELL_GCM_ZPASS_PIXEL_CNT, 600);
    cellGcmSetTimeStamp(&n, 601);
    if (n.current != a + 4 || r.current != b + 2 || memcmp(a, b, 2 * sizeof *a) != 0) fail("FAIL report words");
    if (a[2] != PS3TC_GCM_METHOD(NV40TCL_QUERY_GET, 1) || a[3] != ((1u << 24) | 601u * 0x10u)) fail("FAIL timestamp words");
#ifdef PS3TC_PROF_AUTO
    if (calls != 2 || kinds[0] != 1 || indices[0] != 600 || kinds[1] != 1 || indices[1] != 601) fail("FAIL hook calls");
    puts("ok   observed build: one report hook per inline report write");
#else
    puts("ok   plain build: no hook");
#endif
    return 0;
}
