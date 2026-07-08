#pragma once
// TIM time vocabulary and calendar arithmetic (designed pass).
//
// The calendar math reproduces FMS time_manager.F90 exactly — same date<->day
// mappings (including the Gregorian 400-year-cycle arithmetic and the Julian
// mod-4 leap rule), same increment_date two-mode semantics, same
// normalization/negative-time rules — so DiagManager window boundaries and
// filename stamps land on the same instants FMS would pick. Differences from
// FMS by design:
//   * no module-global calendar: every function takes Calendar explicitly
//     (ensemble members may run distinct configs in one executable);
//   * errors are return codes + message, never FATAL.
// ticks are carried for struct parity with FMS time_type but ticks_per_second
// is fixed at 1 (MOM6 never calls set_ticks_per_second), so ticks==0 always.

#include <string>

namespace TIM {

enum class TimeUnit : int {
  Seconds = 0, Minutes, Hours, Days, Months, Years
};

// Parses "seconds"/"minutes"/"hours"/"days"/"months"/"years" (as used in
// diag_table lines); returns false on anything else.
inline bool parseTimeUnit(const std::string& s, TimeUnit* u) {
  if (s == "seconds") *u = TimeUnit::Seconds;
  else if (s == "minutes") *u = TimeUnit::Minutes;
  else if (s == "hours") *u = TimeUnit::Hours;
  else if (s == "days") *u = TimeUnit::Days;
  else if (s == "months") *u = TimeUnit::Months;
  else if (s == "years") *u = TimeUnit::Years;
  else return false;
  return true;
}

struct DateFields {
  int year = 1, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

// Enumerator values match the FMS time_manager parameters, so the bridge can
// pass fms_get_calendar_type() across bind(C) unchanged.
enum class Calendar : int {
  NoCalendar = 0, ThirtyDayMonths = 1, Julian = 2, Gregorian = 3, NoLeap = 4
};

// Validates and converts an FMS calendar-type integer; false if out of range.
bool calendarFromFms(int fms_type, Calendar* out);

// FMS time_type: an unsigned span since the calendar base date (year 1),
// normalized to 0 <= seconds < 86400. days/seconds are int to inherit FMS
// range behavior.
struct TimeStamp {
  int days = 0, seconds = 0, ticks = 0;

  friend bool operator==(const TimeStamp& a, const TimeStamp& b) {
    return a.days == b.days && a.seconds == b.seconds && a.ticks == b.ticks;
  }
  friend bool operator!=(const TimeStamp& a, const TimeStamp& b) { return !(a == b); }
  friend bool operator<(const TimeStamp& a, const TimeStamp& b) {
    if (a.days != b.days) return a.days < b.days;
    if (a.seconds != b.seconds) return a.seconds < b.seconds;
    return a.ticks < b.ticks;
  }
  friend bool operator>(const TimeStamp& a, const TimeStamp& b) { return b < a; }
  friend bool operator<=(const TimeStamp& a, const TimeStamp& b) { return !(b < a); }
  friend bool operator>=(const TimeStamp& a, const TimeStamp& b) { return !(a < b); }
};

// ---- calendar-free span arithmetic (FMS set_time/increment_time) ----

// Normalizes (seconds, days) into a TimeStamp (floor/modulo carries, exactly
// FMS set_time_private). False + message when the result is negative.
bool makeTime(int seconds, int days, TimeStamp* out, std::string* err);

// t + (seconds, days); negative increments allowed, negative result is an
// error (FMS increment_time_private).
bool incrementTime(const TimeStamp& t, int seconds, int days, TimeStamp* out,
                   std::string* err);

// (to - from) in seconds as double (may be negative); the building block for
// time-axis values ("days since <base>" etc.).
double spanSeconds(const TimeStamp& from, const TimeStamp& to);

// ---- calendar math (FMS set_date/get_date/increment_date) ----

// Date -> span since the calendar base. Validates ranges exactly as FMS
// (year >= 1, day fits the target month incl. leap rules).
bool fromDate(Calendar cal, const DateFields& d, TimeStamp* out,
              std::string* err);

// Span -> date. Fails only for NoCalendar.
bool toDate(Calendar cal, const TimeStamp& t, DateFields* out,
            std::string* err);

// FMS increment_date semantics: EITHER (days/hours/minutes/seconds) OR
// (years/months) may be nonzero, never both. Month arithmetic keeps the
// day-of-month, so e.g. Jan 31 + 1 month is an invalid-date error, exactly
// as FMS. Negative increments allowed (private-version semantics).
bool incrementDate(Calendar cal, const TimeStamp& t, int years, int months,
                   int days, int hours, int minutes, int seconds,
                   TimeStamp* out, std::string* err);

// Days in the month containing t (29 for leap Februaries); 0 for NoCalendar.
int daysInMonth(Calendar cal, const TimeStamp& t);

// Is t inside a leap year? (Gregorian 4/100/400 rule; Julian mod-4; always
// false for ThirtyDayMonths/NoLeap/NoCalendar.)
bool leapYear(Calendar cal, const TimeStamp& t);

// Scheduler helper: t + n*unit. Seconds..Days go through incrementTime,
// Months/Years through incrementDate — the exact recipe FMS diag_util's
// diag_time_inc uses to place output windows.
bool addInterval(Calendar cal, const TimeStamp& t, int n, TimeUnit unit,
                 TimeStamp* out, std::string* err);

}  // namespace TIM
