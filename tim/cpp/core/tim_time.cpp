/**
 * @file tim_time.cpp
 * @brief TIM time concepts and calendar arithmetic implementation.
 * Analogue of FMS time_manager.F90.
 */

#include "tim_time.hpp"

#include "tim_error.hpp"

#include <climits>
#include <cstdio>
#include <string>

namespace TIM {

namespace {

constexpr int kSecondsPerDay = 86400;
constexpr int kDaysPerMonth[13] = {0, 31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};

// Fortran floor(a/real(b)) and modulo(a,b) for b > 0.
int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }

std::string dateStr(const Date& d) {
  // todo: switch to std::format once it's available across compilers.
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", d.year,
                d.month, d.day, d.hour, d.minute, d.second);
  return buf;
}

// Calendar-independent field ranges only (day 1..31); the day-vs-month-length
// check (Feb 30, Apr 31, ...) is in fromDate via monthLength. Also guards the
// kDaysPerMonth[month] indexing below by rejecting month outside 1..12.
void validRanges(const Date& d) {
  if (d.second > 59 || d.second < 0 || d.minute > 59 || d.minute < 0 ||
      d.hour > 23 || d.hour < 0 || d.day > 31 || d.day < 1 || d.month > 12 ||
      d.month < 1 || d.year < 1)
    fatal("invalid date " + dateStr(d));
}

// ---- calendar shape (year >= 1 everywhere) --

bool isLeapYear(Calendar cal, int year) {
  switch (cal) {
    case Calendar::Gregorian:
      return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    case Calendar::Julian:
      return year % 4 == 0;
    default:
      return false;  // NoLeap / ThirtyDayMonths
  }
}

int monthLength(Calendar cal, int year, int month) {
  if (cal == Calendar::ThirtyDayMonths) return 30;
  if (month == 2 && isLeapYear(cal, year)) return 29;
  return kDaysPerMonth[month];
}

// Serial day (0 = 0001-01-01) of the first day of year y: 365 (or 360) days
// per elapsed year plus the leap days among years [1, y-1].
int daysBeforeYear(Calendar cal, int y) {
  const int n = y - 1;
  switch (cal) {
    case Calendar::ThirtyDayMonths: return 360 * n;
    case Calendar::Gregorian:       return 365 * n + n / 4 - n / 100 + n / 400;
    case Calendar::Julian:          return 365 * n + n / 4;
    default:                        return 365 * n;  // NoLeap
  }
}

int daysBeforeMonth(Calendar cal, int year, int month) {
  static constexpr int kCum[13] = {0,   0,   31,  59,  90,  120, 151,
                                   181, 212, 243, 273, 304, 334};
  if (cal == Calendar::ThirtyDayMonths) return 30 * (month - 1);
  return kCum[month] + ((month > 2 && isLeapYear(cal, year)) ? 1 : 0);
}

struct YearDoy {
  int year;  // calendar year (>= 1)
  int doy;   // 1-based day-of-year
};

// Serial day -> (year, day-of-year). Leap calendars repeat exactly in eras
// (Gregorian 400 years / 146097 days, Julian 4 years / 1461 days); the
// year-of-era expressions are the standard closed-form inverse of
// daysBeforeYear (Hinnant's civil-calendar algebra), so no approximation or
// correction step is needed.
YearDoy splitYear(Calendar cal, int days) {
  switch (cal) {
    case Calendar::ThirtyDayMonths:
      return {days / 360 + 1, days % 360 + 1};
    case Calendar::Julian: {
      const int era = days / 1461, doe = days % 1461;
      const int yoe = (doe - doe / 1460) / 365;
      return {era * 4 + yoe + 1, doe - 365 * yoe + 1};
    }
    case Calendar::Gregorian: {
      const int era = days / 146097, doe = days % 146097;
      const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      return {era * 400 + yoe + 1, doe - (365 * yoe + yoe / 4 - yoe / 100) + 1};
    }
    default:  // NoLeap
      return {days / 365 + 1, days % 365 + 1};
  }
}

// Span and calendar increments live in their natural domains -- Time (packed)
// and Date (broken-out) -- rather than one FMS-style increment_date with a
// runtime span-vs-calendar mutual-exclusion check. addInterval bridges via
// toDate/fromDate when the increment is calendar-based.

Time incrementTime(const Time& t, int seconds, int days) {
  if (days > 0 && t.days > INT_MAX - days)
    fatal("integer overflow in days in incrementTime");
  if (seconds > 0 && t.seconds > INT_MAX - seconds)
    fatal("integer overflow in seconds in incrementTime");
  return makeTime(t.seconds + seconds, t.days + days);
}

Date incrementDate(const Date& d, int years, int months) {
  Date r = d;
  r.month += months;
  r.year += floorDiv(r.month - 1, 12);      // FMS: floor((cmonth-1)/12.)
  r.month = floorMod(r.month - 1, 12) + 1;  // FMS: modulo(cmonth-1,12)+1
  r.year += years;
  return r;
}

}  // namespace

Calendar calendarFromFms(int fms_type) {
  if (fms_type < 0 || fms_type > 4)
    fatal("invalid FMS calendar type " + std::to_string(fms_type));
  return static_cast<Calendar>(fms_type);
}

Time makeTime(int seconds, int days) {
  const int days_new = days + floorDiv(seconds, kSecondsPerDay);
  const int seconds_new = floorMod(seconds, kSecondsPerDay);
  if (days_new < 0)
    fatal("negative time: days=" + std::to_string(days_new) +
          " seconds=" + std::to_string(seconds_new));
  return {days_new, seconds_new};
}

double spanSeconds(const Time& from, const Time& to) {
  return (double)(to.days - from.days) * kSecondsPerDay +
         (double)(to.seconds - from.seconds);
}

Time fromDate(Calendar cal, const Date& d) {
  validRanges(d);
  switch (cal) {
    case Calendar::ThirtyDayMonths:
    case Calendar::NoLeap:
    case Calendar::Julian:
    case Calendar::Gregorian:
      break;
    case Calendar::NoCalendar:
      fatal("fromDate is undefined for NO_CALENDAR");
    default:
      fatal("invalid calendar type");
  }
  if (d.day > monthLength(cal, d.year, d.month))
    fatal("invalid date " + dateStr(d));
  Time out;
  out.days = daysBeforeYear(cal, d.year) +
             daysBeforeMonth(cal, d.year, d.month) + d.day - 1;
  out.seconds = d.second + 60 * (d.minute + 60 * d.hour);
  return out;
}

Date toDate(Calendar cal, const Time& t) {
  // FMS time_type is normalized by construction; Time is an open
  // aggregate, so enforce the invariant here instead of decoding garbage.
  if (t.days < 0 || t.seconds < 0 || t.seconds >= kSecondsPerDay)
    fatal("time is not normalized: days=" + std::to_string(t.days) +
          " seconds=" + std::to_string(t.seconds));
  switch (cal) {
    case Calendar::ThirtyDayMonths:
    case Calendar::NoLeap:
    case Calendar::Julian:
    case Calendar::Gregorian:
      break;
    default:
      fatal("toDate is undefined for NO_CALENDAR");
  }
  auto [year, doy] = splitYear(cal, t.days);
  int m = 1;
  while (doy > monthLength(cal, year, m)) {
    doy -= monthLength(cal, year, m);
    ++m;
  }
  Date out;
  out.year = year;
  out.month = m;
  out.day = doy;
  const int isec = t.seconds;
  out.hour = isec / 3600;
  out.minute = (isec - 3600 * out.hour) / 60;
  out.second = isec - 3600 * out.hour - 60 * out.minute;
  return out;
}

Time addInterval(Calendar cal, const Time& t, int n, TimeUnit unit) {
  switch (unit) {
    case TimeUnit::Seconds: return incrementTime(t, n, 0);
    case TimeUnit::Minutes: return incrementTime(t, 60 * n, 0);
    case TimeUnit::Hours:   return incrementTime(t, 3600 * n, 0);
    case TimeUnit::Days:    return incrementTime(t, 0, n);
    case TimeUnit::Months:  return fromDate(cal, incrementDate(toDate(cal, t), 0, n));
    case TimeUnit::Years:   return fromDate(cal, incrementDate(toDate(cal, t), n, 0));
  }
  fatal("invalid time unit");
}

}  // namespace TIM
