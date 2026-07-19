// TIM::Time calendar-math unit tests.
//
// These exercise valid inputs only: the calendar functions abort (amrex::Abort
// -> MPI_Abort) on invalid input, which cannot be unit-tested reliably under
// amrex::Initialize. The valid-input oracle here is the gate.

#include "core/tim_time.hpp"

#include <gtest/gtest.h>

namespace {

using namespace TIM;

Time date(Calendar cal, int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
  return fromDate(cal, {y, mo, d, h, mi, s});
}

void roundtrip(Calendar cal, const char* name, int ndays) {
  for (int day = 0; day < ndays; ++day) {
    const Time t{day, 43200};
    const Date d = toDate(cal, t);
    ASSERT_TRUE(d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31 &&
                d.year >= 1)
        << name << " day=" << day << " gives " << d.year << "-" << d.month
        << "-" << d.day;
    const Time back = fromDate(cal, d);
    ASSERT_EQ(back.days, day) << name << " roundtrip " << d.year << "-"
                              << d.month << "-" << d.day;
    ASSERT_EQ(back.seconds, 43200);
  }
}

// Exhaustive roundtrips over each calendar's full repeat cycle (400 years
// Gregorian, 4 years Julian, 1 year NoLeap/ThirtyDay), plus spill into the
// next cycle to cover the boundary arithmetic.
TEST(Time, RoundtripGregorian) { roundtrip(Calendar::Gregorian, "gregorian", 146097 + 800); }
TEST(Time, RoundtripJulian) { roundtrip(Calendar::Julian, "julian", (4 * 365 + 1) + 800); }
TEST(Time, RoundtripNoLeap) { roundtrip(Calendar::NoLeap, "noleap", 365 + 800); }
TEST(Time, RoundtripThirtyDay) { roundtrip(Calendar::ThirtyDayMonths, "thirty", 360 + 800); }

TEST(Time, EpochIsYear1Jan1) {
  for (Calendar cal : {Calendar::Gregorian, Calendar::Julian, Calendar::NoLeap,
                       Calendar::ThirtyDayMonths}) {
    const Date d = toDate(cal, Time{0, 0});
    EXPECT_EQ(d.year, 1);
    EXPECT_EQ(d.month, 1);
    EXPECT_EQ(d.day, 1);
    EXPECT_EQ(d.hour, 0);
  }
}

TEST(Time, GregorianReferenceSpans) {
  // 1900 is not a leap year (100 rule), 2000 is (400 rule).
  EXPECT_EQ(date(Calendar::Gregorian, 2000, 1, 1).days -
                date(Calendar::Gregorian, 1900, 1, 1).days,
            36524);
  EXPECT_EQ(date(Calendar::Gregorian, 2000, 3, 1).days -
                date(Calendar::Gregorian, 2000, 2, 1).days,
            29);
  // A full 400-year Gregorian cycle is exactly 146097 days.
  EXPECT_EQ(date(Calendar::Gregorian, 2001, 1, 1).days -
                date(Calendar::Gregorian, 1601, 1, 1).days,
            146097);
}

TEST(Time, LeapDayValid) {
  // Gregorian 2000 is a leap year (400 rule): Feb 29 exists.
  EXPECT_EQ(date(Calendar::Gregorian, 2000, 3, 1).days -
                date(Calendar::Gregorian, 2000, 2, 29).days,
            1);
  // Julian is mod-4 only, so 1900 IS a leap year there (unlike Gregorian).
  EXPECT_EQ(date(Calendar::Julian, 1900, 3, 1).days -
                date(Calendar::Julian, 1900, 2, 29).days,
            1);
}

TEST(Time, LinearCalendarYearLengths) {
  EXPECT_EQ(date(Calendar::NoLeap, 245, 1, 1).days, 365 * 244);
  EXPECT_EQ(date(Calendar::ThirtyDayMonths, 2, 1, 1).days, 360);
  // Julian 4-year cycle: years 1-3 common, year 4 leap.
  EXPECT_EQ(date(Calendar::Julian, 5, 1, 1).days, 3 * 365 + 366);
}

TEST(Time, MonthYearIncrements) {
  // Calendar increments via addInterval (the public scheduler face).
  const Time jan15 = date(Calendar::Gregorian, 2000, 1, 15);
  EXPECT_EQ(addInterval(Calendar::Gregorian, jan15, 1, TimeUnit::Years),
            date(Calendar::Gregorian, 2001, 1, 15));
  EXPECT_EQ(addInterval(Calendar::Gregorian, jan15, 2, TimeUnit::Months),
            date(Calendar::Gregorian, 2000, 3, 15));

  const Time dec15 = date(Calendar::Gregorian, 2000, 12, 15, 6);
  EXPECT_EQ(addInterval(Calendar::Gregorian, dec15, 2, TimeUnit::Months),
            date(Calendar::Gregorian, 2001, 2, 15, 6))
      << "+2 months crosses the year and keeps time-of-day";
  EXPECT_EQ(addInterval(Calendar::Gregorian, dec15, -12, TimeUnit::Months),
            date(Calendar::Gregorian, 1999, 12, 15, 6))
      << "-12 months uses floor/modulo month math";
}

TEST(Time, MonthlyWindowWalkNoLeap) {
  // The diag scheduler recipe: monthly boundaries from a base date.
  static const int kOffsets[13] = {0,   31,  59,  90,  120, 151, 181,
                                   212, 243, 273, 304, 334, 365};
  const Time base{0, 0};
  for (int k = 1; k <= 12; ++k) {
    const Time out = addInterval(Calendar::NoLeap, base, k, TimeUnit::Months);
    EXPECT_EQ(out.days, kOffsets[k]) << "+" << k << " months";
  }
  EXPECT_EQ(addInterval(Calendar::NoLeap, base, 3, TimeUnit::Years).days, 3 * 365);
}

TEST(Time, MonthlyWindowWalkGregorianLeapFebruary) {
  const Time base = date(Calendar::Gregorian, 2000, 1, 1);
  const Time out = addInterval(Calendar::Gregorian, base, 2, TimeUnit::Months);
  EXPECT_EQ(out.days - base.days, 31 + 29) << "Feb 2000 has 29 days";
}

TEST(Time, SpanArithmetic) {
  const Time t{1, 86300};
  // Span add/borrow is reached through addInterval (incrementTime is internal).
  EXPECT_EQ(addInterval(Calendar::NoCalendar, t, 200, TimeUnit::Seconds),
            (Time{2, 100})) << "seconds carry into days";
  EXPECT_EQ(addInterval(Calendar::NoCalendar, t, -86400, TimeUnit::Seconds),
            (Time{0, 86300})) << "negative increment borrows";
  EXPECT_EQ(makeTime(90100, 1), (Time{2, 3700})) << "makeTime normalizes";
  EXPECT_EQ(spanSeconds(Time{0, 0}, Time{2, 100}), 172900.0);
  EXPECT_EQ(spanSeconds(Time{2, 100}, Time{0, 0}), -172900.0);
}

TEST(Time, AddIntervalUnits) {
  const Time t{10, 0};
  EXPECT_EQ(addInterval(Calendar::NoCalendar, t, 90, TimeUnit::Minutes),
            (Time{10, 5400})) << "minutes need no calendar";
  EXPECT_EQ(addInterval(Calendar::NoCalendar, t, 36, TimeUnit::Hours),
            (Time{11, 43200})) << "hours carry into days";
  EXPECT_EQ(addInterval(Calendar::NoCalendar, t, 5, TimeUnit::Seconds), (Time{10, 5}));
  EXPECT_EQ(addInterval(Calendar::NoCalendar, t, 3, TimeUnit::Days), (Time{13, 0}));
}

TEST(Time, ComparisonOperators) {
  // operator<=> must reproduce the FMS lexicographic (days, seconds) ordering.
  EXPECT_LT((Time{1, 86399}), (Time{2, 0}));
  EXPECT_LT((Time{2, 10}), (Time{2, 11}));
  EXPECT_EQ((Time{2, 10}), (Time{2, 10}));
  EXPECT_GE((Time{3, 0}), (Time{2, 86399}));
  EXPECT_NE((Time{0, 0}), (Time{0, 1}));
}

TEST(Time, CalendarFromFms) {
  EXPECT_EQ(calendarFromFms(4), Calendar::NoLeap);
  EXPECT_EQ(calendarFromFms(3), Calendar::Gregorian);
  EXPECT_EQ(calendarFromFms(0), Calendar::NoCalendar);
}

}  // namespace
