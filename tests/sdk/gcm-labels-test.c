/* Host test of <cell/gcm/ps3tc_gcm_labels.h>: the reservation tables (A1) and the non-blocking label query with
 * 32-bit serial wrap-around (A4).  Built by gcm-labels-test.sh, also with -DMUTANT_NAIVE (a plain `current >= target`
 * comparison), which must FAIL the wrap cases: the proof that the wrap cases can see the defect.
 *
 * With --dump it prints the reserved ranges as JSON lines for gcm-label-reservation-test.py. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PS3TC_GCM_LABELS_NO_GCM 1
static uint32_t fake_labels[256];
static uint32_t *cellGcmGetLabelAddress(uint8_t index) { return &fake_labels[index]; }
#include <cell/gcm/ps3tc_gcm_labels.h>

#ifdef MUTANT_NAIVE
static int naive_reached(uint32_t c, uint32_t t) { return c >= t; }
#define REACHED(c, t) naive_reached((c), (t))
#else
#define REACHED(c, t) cellGcmIsSerialReached((c), (t))
#endif

static int failures;
static void check(int ok, const char *what)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static int passed(uint8_t index, uint32_t target)
{
#ifdef MUTANT_NAIVE
    return REACHED(cellGcmReadLabelOrdered(cellGcmGetLabelAddress(index)), target);
#else
    return cellGcmIsLabelPassed(index, target);
#endif
}

static int disjoint_sorted(const CellGcmIndexRange *r, uint32_t n, uint32_t limit)
{
    for (uint32_t i = 0; i < n; i++) {
        if (r[i].first > r[i].last || r[i].last >= limit || r[i].reserved != 0 || r[i].owner == 0 ||
            r[i].owner > CELL_GCM_INDEX_OWNER_PROFILER)
            return 0;
        if (i && r[i].first <= r[i - 1].last)
            return 0;
    }
    return 1;
}

static void dump(void)
{
    uint32_t n, i;
    const CellGcmIndexRange *r = cellGcmGetReservedLabelRanges(&n);
    for (i = 0; i < n; i++)
        printf("{\"kind\":\"label\",\"first\":%u,\"last\":%u,\"owner\":%u}\n", r[i].first, r[i].last, r[i].owner);
    r = cellGcmGetReservedReportRanges(&n);
    for (i = 0; i < n; i++)
        printf("{\"kind\":\"report\",\"first\":%u,\"last\":%u,\"owner\":%u}\n", r[i].first, r[i].last, r[i].owner);
    printf("{\"kind\":\"limits\",\"labels\":%u,\"reports\":%u}\n", CELL_GCM_LABEL_COUNT, CELL_GCM_REPORT_LOCAL_COUNT);
}

int main(int argc, char **argv)
{
    uint32_t n;
    const CellGcmIndexRange *r;

    if (argc == 2 && strcmp(argv[1], "--dump") == 0) {
        dump();
        return 0;
    }

    /* ---- A1: the tables ---- */
    r = cellGcmGetReservedLabelRanges(&n);
    check(n == 5 && disjoint_sorted(r, n, 256), "label ranges: 5, ascending, disjoint, in 0..255");
    check(cellGcmGetReservedLabelRanges(NULL) == r, "label table: NULL count allowed, same table");
    r = cellGcmGetReservedReportRanges(&n);
    check(n == 1 && disjoint_sorted(r, n, 2048), "report ranges: 1, in 0..2047");
    {
        uint32_t sys = 0, psgl = 0, prof = 0, app = 0;
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t o = cellGcmGetLabelOwner(i);
            sys += o == CELL_GCM_INDEX_OWNER_SYSTEM;
            psgl += o == CELL_GCM_INDEX_OWNER_PSGL;
            prof += o == CELL_GCM_INDEX_OWNER_PROFILER;
            app += o == 0;
        }
        check(sys == 64 && psgl == 5 && prof == 8 && app == 179, "label owners: 64 system, 5 PSGL, 8 profiler, 179 app");
    }
    check(cellGcmGetLabelOwner(64) == 0 && cellGcmGetLabelOwner(69) == 0 &&
              cellGcmGetLabelOwner(239) == 0 && cellGcmGetLabelOwner(248) == 0 &&
              cellGcmGetLabelOwner(254) == 0,
          "application labels: 64, 69, 239, 248, 254");
    check(cellGcmGetLabelOwner(63) == CELL_GCM_INDEX_OWNER_SYSTEM && cellGcmGetLabelOwner(65) == CELL_GCM_INDEX_OWNER_PSGL &&
              cellGcmGetLabelOwner(68) == CELL_GCM_INDEX_OWNER_PSGL &&
              cellGcmGetLabelOwner(240) == CELL_GCM_INDEX_OWNER_PROFILER &&
              cellGcmGetLabelOwner(247) == CELL_GCM_INDEX_OWNER_PROFILER &&
              cellGcmGetLabelOwner(255) == CELL_GCM_INDEX_OWNER_PSGL,
          "reserved edges: 63, 65, 68, 240, 247, 255");
    {
        int app_block = 1;
        for (uint32_t i = CELL_GCM_LABEL_APP_FIRST; i <= CELL_GCM_LABEL_APP_LAST; i++)
            app_block &= cellGcmGetLabelOwner(i) == 0;
        check(app_block && cellGcmGetLabelOwner(CELL_GCM_LABEL_APP_FIRST - 1) != 0 &&
                  cellGcmGetLabelOwner(CELL_GCM_LABEL_APP_LAST + 1) != 0,
              "CELL_GCM_LABEL_APP_FIRST..LAST is exactly a maximal free block");
    }
    check(cellGcmGetReportOwner(0) == CELL_GCM_INDEX_OWNER_PROFILER &&
              cellGcmGetReportOwner(511) == CELL_GCM_INDEX_OWNER_PROFILER && cellGcmGetReportOwner(512) == 0 &&
              cellGcmGetReportOwner(2047) == 0,
          "reports: 0..511 profiler, 512..2047 app");

    /* ---- A4: serial arithmetic, including the wrap boundary ---- */
    check(REACHED(5u, 5u) && REACHED(6u, 5u) && !REACHED(4u, 5u), "plain: equal and later reached, earlier not");
    check(REACHED(0u, 0xffffffffu), "wrap: 0 has reached 0xffffffff");
    check(REACHED(1u, 0xfffffffeu), "wrap: 1 has reached 0xfffffffe");
    check(!REACHED(0xffffffffu, 0u), "wrap: 0xffffffff has not reached 0");
    check(!REACHED(0xfffffffeu, 2u), "wrap: 0xfffffffe has not reached 2");
    check(REACHED(0x7fffffffu, 0u) && !REACHED(0x80000000u, 0u) && REACHED(0u, 0x80000001u),
          "distance edge: 2^31-1 ahead reached; 2^31 is the wrap-around answer");
    {
        /* a writer that advances one label by one per frame across the wrap; the reader polls each target */
        int ok = 1;
        uint32_t start = 0xfffffff0u;
        for (uint32_t k = 0; k < 64; k++) {
            uint32_t written = start + k; /* wraps after 16 steps */
            fake_labels[200] = written;
            for (uint32_t d = 0; d < 8; d++) {
                ok &= passed(200, written - d) == 1;          /* every target at or before the label */
                ok &= passed(200, written + 1u + d) == 0;     /* every target after it */
            }
        }
        check(ok, "label 200 across 0xfffffff0..0x2f: passed exactly the reached targets");
    }
    fake_labels[100] = 0u;
    check(passed(100, 0u) && !passed(100, 1u), "label read: no side effect on the label (still 0)");
    check(fake_labels[100] == 0u, "label word unchanged by the query");

    printf("gcm-labels-test: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
