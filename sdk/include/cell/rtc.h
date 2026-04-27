/*! \file cell/rtc.h
 \brief cellRtc real-time-clock API.

  Reference-SDK-compatible cellRtc surface backed by the
  librtc_stub.a archive emitted from
  tools/nidgen/nids/extracted/librtc_stub.yaml. Link with
  `-lrtc_stub`.

  Two time representations:
    - CellRtcDateTime  — fielded (year/month/day/hour/min/sec/us)
    - CellRtcTick      — single 64-bit microsecond count from
                         year-0001 / month-01 / day-01 / 00:00:00

  All entry points return 0 on success; non-zero is one of the
  CELL_RTC_ERROR_* codes below.
*/

#ifndef __PSL1GHT_CELL_RTC_H__
#define __PSL1GHT_CELL_RTC_H__

#include <stdint.h>
#include <sys/time.h>   /* time_t */
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes ----------------------------------------------------- */

#define CELL_RTC_ERROR_NOT_INITIALIZED        0x80010601
#define CELL_RTC_ERROR_INVALID_POINTER        0x80010602
#define CELL_RTC_ERROR_INVALID_VALUE          0x80010603
#define CELL_RTC_ERROR_INVALID_ARG            0x80010604
#define CELL_RTC_ERROR_NOT_SUPPORTED          0x80010605
#define CELL_RTC_ERROR_NO_CLOCK               0x80010606
#define CELL_RTC_ERROR_BAD_PARSE              0x80010607
#define CELL_RTC_ERROR_INVALID_YEAR           0x80010621
#define CELL_RTC_ERROR_INVALID_MONTH          0x80010622
#define CELL_RTC_ERROR_INVALID_DAY            0x80010623
#define CELL_RTC_ERROR_INVALID_HOUR           0x80010624
#define CELL_RTC_ERROR_INVALID_MINUTE         0x80010625
#define CELL_RTC_ERROR_INVALID_SECOND        0x80010626
#define CELL_RTC_ERROR_INVALID_MICROSECOND    0x80010627

/* ---- day-of-week codes (returned by cellRtcGetDayOfWeek) ------------ */

#define CELL_RTC_DAYOFWEEK_SUNDAY     0
#define CELL_RTC_DAYOFWEEK_MONDAY     1
#define CELL_RTC_DAYOFWEEK_TUESDAY    2
#define CELL_RTC_DAYOFWEEK_WEDNESDAY  3
#define CELL_RTC_DAYOFWEEK_THURSDAY   4
#define CELL_RTC_DAYOFWEEK_FRIDAY     5
#define CELL_RTC_DAYOFWEEK_SATURDAY   6

/* ---- representations -------------------------------------------------- */

typedef struct CellRtcDateTime {
	unsigned short year;
	unsigned short month;
	unsigned short day;
	unsigned short hour;
	unsigned short minute;
	unsigned short second;
	unsigned int   microsecond;
} CellRtcDateTime;

typedef struct CellRtcTick {
	uint64_t tick;
} CellRtcTick;

/* ---- current-time queries --------------------------------------------- */

int cellRtcGetCurrentTick(CellRtcTick *pTick);
int cellRtcGetCurrentClock(CellRtcDateTime *pTime, int iTimeZone);
int cellRtcGetCurrentClockLocalTime(CellRtcDateTime *pTime);

/* Convenience macros mirrored from the reference surface. */
#define cellRtcGetCurrentTickUtc(_tick)   cellRtcGetCurrentTick((_tick))
#define cellRtcGetCurrentClockUtc(_p)     cellRtcGetCurrentClock((_p), 0)

/* ---- UTC <-> local-time conversion ------------------------------------ */

int cellRtcConvertUtcToLocalTime(const CellRtcTick *pUtc, CellRtcTick *pLocalTime);
int cellRtcConvertLocalTimeToUtc(const CellRtcTick *pLocalTime, CellRtcTick *pUtc);

/* ---- calendar helpers ------------------------------------------------- */

int cellRtcIsLeapYear(int year);
int cellRtcGetDaysInMonth(int year, int month);
int cellRtcGetDayOfWeek(int year, int month, int day);
int cellRtcCheckValid(const CellRtcDateTime *pTime);

/* ---- DateTime <-> external-time-format conversion --------------------- */

int cellRtcSetTime_t      (CellRtcDateTime *pTime, time_t llTime);
int cellRtcGetTime_t      (const CellRtcDateTime *pTime, time_t *pllTime);
int cellRtcSetDosTime     (CellRtcDateTime *pTime, unsigned int uiDosTime);
int cellRtcGetDosTime     (const CellRtcDateTime *pTime, unsigned int *puiDosTime);
int cellRtcSetWin32FileTime(CellRtcDateTime *pTime, uint64_t ulWin32Time);
int cellRtcGetWin32FileTime(const CellRtcDateTime *pTime, uint64_t *ulWin32Time);

#define cellRtcConvertTime_tToDateTime(_t, _pdt)   cellRtcSetTime_t((_pdt), (_t))
#define cellRtcConvertDateTimeToTime_t(_pdt, _pt)  cellRtcGetTime_t((_pdt), (_pt))
#define cellRtcConvertDosTimeToDateTime(_d, _pdt)  cellRtcSetDosTime((_pdt), (_d))
#define cellRtcConvertDateTimeToDosTime(_pdt, _pd) cellRtcGetDosTime((_pdt), (_pd))
#define cellRtcConvertWin32TimeToDateTime(_pw, _pdt) cellRtcSetWin32FileTime((_pdt), (_pw))
#define cellRtcConvertDateTimeToWin32Time(_pdt, _pw) cellRtcGetWin32FileTime((_pdt), (_pw))

/* ---- DateTime <-> Tick conversion ------------------------------------- */

int cellRtcSetTick(CellRtcDateTime *pTime, const CellRtcTick *pTick);
int cellRtcGetTick(const CellRtcDateTime *pTime, CellRtcTick *pTick);

#define cellRtcConvertTickToDateTime(_pt, _pdt) cellRtcSetTick((_pdt), (_pt))
#define cellRtcConvertDateTimeToTick(_pdt, _pt) cellRtcGetTick((_pdt), (_pt))

int cellRtcCompareTick(const CellRtcTick *pTick1, const CellRtcTick *pTick2);

/* ---- tick arithmetic -------------------------------------------------- */

int cellRtcTickAddTicks       (CellRtcTick *pOut, const CellRtcTick *pIn, int64_t lAdd);
int cellRtcTickAddMicroseconds(CellRtcTick *pOut, const CellRtcTick *pIn, int64_t lAdd);
int cellRtcTickAddSeconds     (CellRtcTick *pOut, const CellRtcTick *pIn, int64_t lAdd);
int cellRtcTickAddMinutes     (CellRtcTick *pOut, const CellRtcTick *pIn, int64_t lAdd);
int cellRtcTickAddHours       (CellRtcTick *pOut, const CellRtcTick *pIn, int     lAdd);
int cellRtcTickAddDays        (CellRtcTick *pOut, const CellRtcTick *pIn, int     lAdd);
int cellRtcTickAddWeeks       (CellRtcTick *pOut, const CellRtcTick *pIn, int     lAdd);
int cellRtcTickAddMonths      (CellRtcTick *pOut, const CellRtcTick *pIn, int     lAdd);
int cellRtcTickAddYears       (CellRtcTick *pOut, const CellRtcTick *pIn, int     lAdd);

/* ---- formatting / parsing -------------------------------------------- */

int cellRtcFormatRfc2822         (char *pszDateTime, const CellRtcTick *pUtc, int iTimeZoneMinutes);
int cellRtcFormatRfc2822LocalTime(char *pszDateTime, const CellRtcTick *pUtc);
int cellRtcFormatRfc3339         (char *pszDateTime, const CellRtcTick *pUtc, int iTimeZoneMinutes);
int cellRtcFormatRfc3339LocalTime(char *pszDateTime, const CellRtcTick *pUtc);
int cellRtcParseDateTime         (CellRtcTick *pUtc, const char *pszDateTime);
int cellRtcParseRfc3339          (CellRtcTick *pUtc, const char *pszDateTime);

/* ---- field setters / getters (header-resident, no SPRX call) --------- */

static inline int cellRtcSetYear(CellRtcDateTime *pTime, int year) {
	if (year < 1 || year > 9999) return CELL_RTC_ERROR_INVALID_ARG;
	pTime->year = (unsigned short)year;
	return 0;
}
static inline int cellRtcSetMonth(CellRtcDateTime *pTime, int month) {
	if (month < 1 || month > 12) return CELL_RTC_ERROR_INVALID_ARG;
	pTime->month = (unsigned short)month;
	return 0;
}
static inline int cellRtcSetDay(CellRtcDateTime *pTime, int day) {
	if (day < 1 || day > 31) return CELL_RTC_ERROR_INVALID_ARG;
	pTime->day = (unsigned short)day;
	return 0;
}
static inline int cellRtcSetHour(CellRtcDateTime *pTime, int hour) {
	if (hour < 0 || hour > 23) return CELL_RTC_ERROR_INVALID_ARG;
	pTime->hour = (unsigned short)hour;
	return 0;
}
static inline int cellRtcSetMinute(CellRtcDateTime *pTime, int minute) {
	if (minute < 0 || minute > 59) return CELL_RTC_ERROR_INVALID_ARG;
	pTime->minute = (unsigned short)minute;
	return 0;
}
static inline int cellRtcSetSecond(CellRtcDateTime *pTime, int second) {
	if (second < 0 || second > 59) return CELL_RTC_ERROR_INVALID_ARG;
	pTime->second = (unsigned short)second;
	return 0;
}
static inline int cellRtcSetMicrosecond(CellRtcDateTime *pTime, int microsecond) {
	if (microsecond < 0 || microsecond > 999999) return CELL_RTC_ERROR_INVALID_ARG;
	pTime->microsecond = (unsigned int)microsecond;
	return 0;
}

static inline int cellRtcGetYear        (const CellRtcDateTime *pTime) { return pTime->year; }
static inline int cellRtcGetMonth       (const CellRtcDateTime *pTime) { return pTime->month; }
static inline int cellRtcGetDay         (const CellRtcDateTime *pTime) { return pTime->day; }
static inline int cellRtcGetHour        (const CellRtcDateTime *pTime) { return pTime->hour; }
static inline int cellRtcGetMinute      (const CellRtcDateTime *pTime) { return pTime->minute; }
static inline int cellRtcGetSecond      (const CellRtcDateTime *pTime) { return pTime->second; }
static inline int cellRtcGetMicrosecond (const CellRtcDateTime *pTime) { return (int)pTime->microsecond; }

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_RTC_H__ */
