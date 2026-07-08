// TIM::Time calendar-math unit checks. The FMS time_manager algorithms were
// ported literally; these tests pin (a) internal consistency (exhaustive
// toDate/fromDate roundtrips over full calendar cycles), (b) hand-computed
// FMS reference values, (c) increment_date two-mode semantics.
#include "../../tim/cpp/core/tim_time.hpp"

#include <cstdio>

using namespace TIM;

static int failures = 0;
#define CHECK(cond, what)                                        \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
      ++failures;                                                \
    }                                                            \
  } while (0)

static void roundtrip(Calendar cal, const char* name, int ndays) {
  std::string err;
  for (int day = 0; day < ndays; ++day) {
    TimeStamp t{day, 43200, 0};
    DateFields d;
    if (!toDate(cal, t, &d, &err)) {
      std::printf("FAIL %s toDate day=%d: %s\n", name, day, err.c_str());
      ++failures;
      return;
    }
    if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > 31 || d.year < 1) {
      std::printf("FAIL %s toDate day=%d gives %d-%d-%d\n", name, day, d.year,
                  d.month, d.day);
      ++failures;
      return;
    }
    TimeStamp back;
    if (!fromDate(cal, d, &back, &err) || back.days != day ||
        back.seconds != 43200) {
      std::printf("FAIL %s roundtrip day=%d -> %d-%02d-%02d -> day=%d (%s)\n",
                  name, day, d.year, d.month, d.day, back.days, err.c_str());
      ++failures;
      return;
    }
  }
}

int main() {
  std::string err;

  // (a) exhaustive roundtrips: Gregorian over a full 400-year cycle plus
  // spill into the next; the others over 400+ years.
  roundtrip(Calendar::Gregorian, "gregorian", 146097 + 800);
  roundtrip(Calendar::Julian, "julian", 366 * 401);
  roundtrip(Calendar::NoLeap, "noleap", 365 * 401);
  roundtrip(Calendar::ThirtyDayMonths, "thirty", 360 * 401);

  {  // (b) epoch: day 0 is 0001-01-01 in every calendar
    for (Calendar cal : {Calendar::Gregorian, Calendar::Julian,
                         Calendar::NoLeap, Calendar::ThirtyDayMonths}) {
      DateFields d;
      CHECK(toDate(cal, TimeStamp{0, 0, 0}, &d, &err), "epoch toDate");
      CHECK(d.year == 1 && d.month == 1 && d.day == 1 && d.hour == 0,
            "epoch is 0001-01-01");
    }
  }

  {  // (b) Gregorian reference spans
    TimeStamp a, b;
    CHECK(fromDate(Calendar::Gregorian, {1900, 1, 1, 0, 0, 0}, &a, &err), "g1900");
    CHECK(fromDate(Calendar::Gregorian, {2000, 1, 1, 0, 0, 0}, &b, &err), "g2000");
    CHECK(b.days - a.days == 36524, "1900->2000 = 36524 days (1900 not leap)");
    CHECK(fromDate(Calendar::Gregorian, {2000, 3, 1, 0, 0, 0}, &b, &err), "g2000mar");
    CHECK(fromDate(Calendar::Gregorian, {2000, 2, 1, 0, 0, 0}, &a, &err), "g2000feb");
    CHECK(b.days - a.days == 29, "Feb 2000 has 29 days (400 rule)");
    CHECK(!fromDate(Calendar::Gregorian, {1900, 2, 29, 0, 0, 0}, &a, &err),
          "1900-02-29 invalid (100 rule)");
    CHECK(fromDate(Calendar::Julian, {1900, 2, 29, 0, 0, 0}, &a, &err),
          "1900-02-29 VALID in Julian (mod-4 only)");
  }

  {  // (b) leapYear / daysInMonth
    TimeStamp t;
    CHECK(fromDate(Calendar::Gregorian, {2100, 6, 1, 0, 0, 0}, &t, &err), "g2100");
    CHECK(!leapYear(Calendar::Gregorian, t), "2100 not leap");
    CHECK(fromDate(Calendar::Gregorian, {2004, 2, 10, 0, 0, 0}, &t, &err), "g2004");
    CHECK(leapYear(Calendar::Gregorian, t), "2004 leap");
    CHECK(daysInMonth(Calendar::Gregorian, t) == 29, "Feb 2004 = 29");
    CHECK(fromDate(Calendar::NoLeap, {2004, 2, 10, 0, 0, 0}, &t, &err), "nl2004");
    CHECK(daysInMonth(Calendar::NoLeap, t) == 28, "noleap Feb = 28");
    CHECK(daysInMonth(Calendar::ThirtyDayMonths, t) == 30, "thirty = 30");
  }

  {  // (b) noleap year offsets are exact multiples of 365
    TimeStamp t;
    CHECK(fromDate(Calendar::NoLeap, {245, 1, 1, 0, 0, 0}, &t, &err), "nl245");
    CHECK(t.days == 365 * 244, "noleap linear years");
  }

  {  // (c) increment_date: mode separation
    TimeStamp t{0, 0, 0}, out;
    CHECK(!incrementDate(Calendar::NoLeap, t, 0, 1, 1, 0, 0, 0, &out, &err),
          "months+days rejected");
    CHECK(incrementDate(Calendar::NoLeap, t, 0, 0, 0, 0, 0, 0, &out, &err) &&
              out == t,
          "all-zero increment = identity");
  }

  {  // (c) month rollover, forward and backward
    TimeStamp t, out;
    DateFields d;
    CHECK(fromDate(Calendar::Gregorian, {2000, 12, 15, 6, 0, 0}, &t, &err), "dec15");
    CHECK(incrementDate(Calendar::Gregorian, t, 0, 2, 0, 0, 0, 0, &out, &err),
          "+2 months");
    CHECK(toDate(Calendar::Gregorian, out, &d, &err) && d.year == 2001 &&
              d.month == 2 && d.day == 15 && d.hour == 6,
          "2000-12-15 +2mo = 2001-02-15, time-of-day kept");
    CHECK(incrementDate(Calendar::Gregorian, t, 0, -12, 0, 0, 0, 0, &out, &err),
          "-12 months");
    CHECK(toDate(Calendar::Gregorian, out, &d, &err) && d.year == 1999 &&
              d.month == 12 && d.day == 15,
          "-12mo crosses year (floor/modulo month math)");
  }

  {  // (c) FMS quirk kept: day-of-month preserved, so Jan 31 + 1 month errors
    TimeStamp t, out;
    CHECK(fromDate(Calendar::Gregorian, {2001, 1, 31, 0, 0, 0}, &t, &err), "jan31");
    CHECK(!incrementDate(Calendar::Gregorian, t, 0, 1, 0, 0, 0, 0, &out, &err),
          "Jan 31 + 1 month = invalid date (FMS behavior)");
  }

  {  // (c) monthly window walk from a CESM-style base date (noleap)
    TimeStamp t{0, 0, 0}, out;
    static const int kOffsets[13] = {0, 31, 59, 90, 120, 151, 181,
                                     212, 243, 273, 304, 334, 365};
    bool ok = true;
    for (int k = 1; k <= 12; ++k) {
      if (!addInterval(Calendar::NoLeap, t, k, TimeUnit::Months, &out, &err) ||
          out.days != kOffsets[k])
        ok = false;
    }
    CHECK(ok, "noleap monthly boundaries from 0001-01-01");
    CHECK(addInterval(Calendar::NoLeap, t, 3, TimeUnit::Years, &out, &err) &&
              out.days == 3 * 365,
          "noleap +3 years");
  }

  {  // span arithmetic: carries, borrows, negative-result error
    TimeStamp t{1, 86300, 0}, out;
    CHECK(incrementTime(t, 200, 0, &out, &err) && out.days == 2 &&
              out.seconds == 100,
          "second->day carry");
    CHECK(incrementTime(t, -86400, 0, &out, &err) && out.days == 0 &&
              out.seconds == 86300,
          "negative increment borrows");
    CHECK(!incrementTime(TimeStamp{0, 10, 0}, -20, 0, &out, &err),
          "negative time rejected");
    CHECK(spanSeconds(TimeStamp{0, 0, 0}, TimeStamp{2, 100, 0}) == 172900.0,
          "spanSeconds");
  }

  {  // hour/minute/second increments via addInterval
    TimeStamp t{10, 0, 0}, out;
    CHECK(addInterval(Calendar::NoCalendar, t, 90, TimeUnit::Minutes, &out,
                      &err) &&
              out.days == 10 && out.seconds == 5400,
          "minutes need no calendar");
    CHECK(addInterval(Calendar::NoCalendar, t, 36, TimeUnit::Hours, &out,
                      &err) &&
              out.days == 11 && out.seconds == 43200,
          "hours carry into days");
    CHECK(!addInterval(Calendar::NoCalendar, t, 1, TimeUnit::Months, &out, &err),
          "months on NO_CALENDAR rejected");
  }

  {  // calendarFromFms mapping
    Calendar c;
    CHECK(calendarFromFms(4, &c) && c == Calendar::NoLeap, "FMS NOLEAP=4");
    CHECK(calendarFromFms(3, &c) && c == Calendar::Gregorian, "FMS GREGORIAN=3");
    CHECK(!calendarFromFms(7, &c), "out-of-range rejected");
  }

  if (failures == 0) std::printf("ALL TIME CHECKS PASS\n");
  return failures == 0 ? 0 : 1;
}
