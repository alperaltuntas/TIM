#pragma once
/**
 * @file tim_time.hpp
 * @brief TIM time concepts and calendar arithmetic.
 *
 * The calendar math and logic is based on FMS time_manager.F90,
 * including the error handling (abort on invalid input).
 * Differences from FMS by design:
 *   - no singleton or module-global calendar (every function takes Calendar).
 *   - no ticks field: production MOM6 never calls set_ticks_per_second (only its
 *     file-parser unit tests do).
 */

#include <compare>

// Here, we don't introduce a TIM::Time namespace or class, because the
// vocabulary is flat and value-based, not a stateful service subsystem.
namespace TIM {

/// @brief Time units for interval arithmetic (see addInterval).
enum class TimeUnit : int {
  Seconds = 0,  ///< Seconds.
  Minutes,      ///< Minutes.
  Hours,        ///< Hours.
  Days,         ///< Days.
  Months,       ///< Calendar months (calendar-dependent length).
  Years         ///< Calendar years (calendar-dependent length).
};

/// @brief A broken-out calendar date and time-of-day (the FMS get_date/set_date
/// fields: year..second). "Date" includes h:m:s, matching FMS get_date.
struct Date {
  int year = 1;    ///< Calendar year (>= 1; there is no year 0).
  int month = 1;   ///< Month of year, 1..12.
  int day = 1;     ///< Day of month, 1-based.
  int hour = 0;    ///< Hour of day, 0..23.
  int minute = 0;  ///< Minute of hour, 0..59.
  int second = 0;  ///< Second of minute, 0..59.
};

/// @brief Supported calendars.
///
/// Enumerator values match the FMS time_manager parameters, so the bridge
/// can pass fms_get_calendar_type() across bind(C) unchanged.
enum class Calendar : int {
  NoCalendar = 0,       ///< Pure time spans; no date arithmetic.
  ThirtyDayMonths = 1,  ///< Twelve 30-day months.
  Julian = 2,           ///< Every year divisible by 4 is leap.
  Gregorian = 3,        ///< Gregorian 4/100/400 leap rule.
  NoLeap = 4            ///< 365-day years, no leap days.
};

/// @brief Maps an FMS calendar-type integer to a Calendar.
/// @param fms_type FMS calendar parameter value (0..4).
/// @return The matching calendar.
/// @note Rank-local. Aborts on an out-of-range integer.
Calendar calendarFromFms(int fms_type);

/// @brief FMS time_type: an unsigned span since the calendar base date
/// (year 1), normalized to 0 <= seconds < 86400.
struct Time {
  int days = 0;     ///< Whole days of the span.
  int seconds = 0;  ///< Seconds into the day, 0..86399.

  /// @brief Lexicographic (days, seconds) ordering.
  friend constexpr auto operator<=>(const Time&, const Time&) = default;
};

// ---- calendar-free span arithmetic (FMS set_time/increment_time) ----

/// @brief Normalizes (seconds, days) into a Time.
/// @param seconds Second count; may exceed a day or be negative.
/// @param days Day count.
/// @return The normalized stamp.
/// @note Rank-local. Aborts on a negative result.
Time makeTime(int seconds, int days);

/// @brief (to - from) in seconds; the building block for time-axis values
/// ("days since <base>" etc.).
/// @param from Span start.
/// @param to Span end.
/// @return Signed span in seconds as double.
/// @note Rank-local; never fails.
double spanSeconds(const Time& from, const Time& to);

// ---- calendar math (FMS set_date/get_date/increment_date) ----
//
// Free functions over an explicit Calendar, not methods of a Calendar class:
// Calendar is a stateless tag (nothing to encapsulate), the natural receiver
// is ambiguous (calendar vs. Time), and the bind(C) seam needs plain
// functions over the int-valued Calendar anyway.

/// @brief Date -> span since the calendar base (FMS set_date).
/// @param cal Calendar to interpret the date in.
/// @param d The date to convert.
/// @return The resulting stamp.
/// @note Rank-local. Aborts on an invalid date.
Time fromDate(Calendar cal, const Date& d);

/// @brief Span -> date (FMS get_date).
/// @param cal Calendar to express the date in.
/// @param t The stamp to convert; must be normalized.
/// @return The resulting date.
/// @note Rank-local. Aborts on NoCalendar or an unnormalized stamp.
Date toDate(Calendar cal, const Time& t);

/// @brief Scheduler helper: t + n*unit.
///
/// Seconds..Days advance the span directly, Months/Years advance the calendar
/// date (keeping the day-of-month).
/// @param cal Calendar for month/year units.
/// @param t Base time.
/// @param n Interval count; negative allowed.
/// @param unit Interval unit.
/// @return The advanced stamp.
/// @note Rank-local. Aborts if the underlying increment is invalid.
Time addInterval(Calendar cal, const Time& t, int n, TimeUnit unit);

}  // namespace TIM
