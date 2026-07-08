#pragma once
// TIM time vocabulary (designed pass). Calendar arithmetic (advance/toDate,
// month/year intervals, filename date stamping) is added when the diag
// manager's scheduler needs it; the value types live here from the start so
// every component shares one time language.

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

}  // namespace TIM
