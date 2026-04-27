/*
 * hello-ppu-rtc — librtc validation test.
 *
 * Loads the cellRtc SPRX, exercises the most commonly used entry
 * points, and prints results. Validates that:
 *   - the librtc_stub.a archive's FNID slots resolve at runtime,
 *   - DateTime/Tick conversion roundtrips,
 *   - tick arithmetic walks a known input as expected,
 *   - calendar helpers (leap year / days in month / day-of-week)
 *     return correct values for hand-checked inputs.
 *
 * If every check prints "OK", the cellRtc surface is end-to-end
 * functional through our toolchain.
 */

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include <sys/process.h>
#include <cell/sysmodule.h>
#include <cell/rtc.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define EXPECT_EQ(label, got, want)                                     \
    do {                                                                \
        long long _g = (long long)(got), _w = (long long)(want);        \
        if (_g == _w) printf("  OK   %-44s = %lld\n", label, _g);       \
        else { printf("  FAIL %-44s = %lld (want %lld)\n",              \
                      label, _g, _w); ++failed; }                       \
    } while (0)

#define EXPECT_OK(label, expr)                                          \
    do {                                                                \
        int _r = (expr);                                                \
        if (_r == 0) printf("  OK   %s\n", label);                      \
        else { printf("  FAIL %s = 0x%08x\n", label, _r); ++failed; }   \
    } while (0)

int main(void)
{
    int failed = 0;

    printf("hello-ppu-rtc: librtc validation test\n");

    /* librtc lives in cellRtc.sprx — must be loaded before we can call. */
    EXPECT_OK("cellSysmoduleLoadModule(CELL_SYSMODULE_RTC)",
              cellSysmoduleLoadModule(CELL_SYSMODULE_RTC));

    /* --- current-time queries ---------------------------------------- */
    CellRtcTick now_tick = {0};
    EXPECT_OK("cellRtcGetCurrentTick", cellRtcGetCurrentTick(&now_tick));
    printf("       now_tick = %" PRIu64 "\n", now_tick.tick);

    CellRtcDateTime now_clock = {0};
    EXPECT_OK("cellRtcGetCurrentClock(UTC)",
              cellRtcGetCurrentClock(&now_clock, 0));
    printf("       now_clock = %04u-%02u-%02u %02u:%02u:%02u.%06u\n",
           now_clock.year, now_clock.month, now_clock.day,
           now_clock.hour, now_clock.minute, now_clock.second,
           now_clock.microsecond);

    /* --- DateTime <-> Tick roundtrip --------------------------------- */
    CellRtcTick rt_tick = {0};
    EXPECT_OK("cellRtcGetTick(now_clock)",
              cellRtcGetTick(&now_clock, &rt_tick));
    CellRtcDateTime rt_clock = {0};
    EXPECT_OK("cellRtcSetTick(rt_tick)",
              cellRtcSetTick(&rt_clock, &rt_tick));
    int eq = (rt_clock.year   == now_clock.year   &&
              rt_clock.month  == now_clock.month  &&
              rt_clock.day    == now_clock.day    &&
              rt_clock.hour   == now_clock.hour   &&
              rt_clock.minute == now_clock.minute &&
              rt_clock.second == now_clock.second);
    EXPECT_EQ("DateTime->Tick->DateTime fields equal", eq, 1);

    /* --- calendar helpers, hand-verified --------------------------- */
    EXPECT_EQ("cellRtcIsLeapYear(2000)",   cellRtcIsLeapYear(2000), 1);
    EXPECT_EQ("cellRtcIsLeapYear(1900)",   cellRtcIsLeapYear(1900), 0);
    EXPECT_EQ("cellRtcIsLeapYear(2024)",   cellRtcIsLeapYear(2024), 1);
    EXPECT_EQ("cellRtcGetDaysInMonth(2024, 2) leap-Feb",
              cellRtcGetDaysInMonth(2024, 2), 29);
    EXPECT_EQ("cellRtcGetDaysInMonth(2023, 2) common-Feb",
              cellRtcGetDaysInMonth(2023, 2), 28);
    EXPECT_EQ("cellRtcGetDaysInMonth(2024, 4) Apr",
              cellRtcGetDaysInMonth(2024, 4), 30);
    /* 2024-04-26 was a Friday. */
    EXPECT_EQ("cellRtcGetDayOfWeek(2024,4,26) = FRIDAY",
              cellRtcGetDayOfWeek(2024, 4, 26),
              CELL_RTC_DAYOFWEEK_FRIDAY);

    /* --- tick arithmetic ------------------------------------------- */
    CellRtcTick t_plus_1d = {0};
    EXPECT_OK("cellRtcTickAddDays(+1)",
              cellRtcTickAddDays(&t_plus_1d, &now_tick, 1));
    long long delta_us = (long long)(t_plus_1d.tick - now_tick.tick);
    EXPECT_EQ("AddDays(+1) delta == 86400 * 1e6 us",
              delta_us, 86400LL * 1000000LL);

    CellRtcTick t_plus_1h = {0};
    EXPECT_OK("cellRtcTickAddHours(+1)",
              cellRtcTickAddHours(&t_plus_1h, &now_tick, 1));
    EXPECT_EQ("AddHours(+1) delta == 3600 * 1e6 us",
              (long long)(t_plus_1h.tick - now_tick.tick),
              3600LL * 1000000LL);

    /* --- formatting ------------------------------------------------ */
    char rfc3339[64] = {0};
    EXPECT_OK("cellRtcFormatRfc3339(now_tick)",
              cellRtcFormatRfc3339(rfc3339, &now_tick, 0));
    printf("       rfc3339 = \"%s\"\n", rfc3339);

    /* --- field setters / inline helpers ---------------------------- */
    CellRtcDateTime dt = {0};
    EXPECT_OK("cellRtcSetYear(2024)",  cellRtcSetYear(&dt, 2024));
    EXPECT_OK("cellRtcSetMonth(4)",    cellRtcSetMonth(&dt, 4));
    EXPECT_OK("cellRtcSetDay(26)",     cellRtcSetDay(&dt, 26));
    EXPECT_OK("cellRtcSetHour(12)",    cellRtcSetHour(&dt, 12));
    EXPECT_OK("cellRtcSetMinute(30)",  cellRtcSetMinute(&dt, 30));
    EXPECT_OK("cellRtcSetSecond(15)",  cellRtcSetSecond(&dt, 15));
    EXPECT_OK("cellRtcCheckValid",     cellRtcCheckValid(&dt));
    EXPECT_EQ("cellRtcGetYear", cellRtcGetYear(&dt), 2024);

    cellSysmoduleUnloadModule(CELL_SYSMODULE_RTC);

    if (failed) printf("hello-ppu-rtc: %d failure(s)\n", failed);
    else        printf("hello-ppu-rtc: all checks passed\n");
    return failed ? 1 : 0;
}
