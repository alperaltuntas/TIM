#include "tim_time.hpp"

#include <cstdio>

namespace TIM {

namespace {

constexpr int kSecondsPerDay = 86400;
constexpr int kDaysIn400Years = 146097;  // Gregorian only
constexpr int kDaysPerMonth[13] = {0, 31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};

// Fortran floor(a/real(b)) and modulo(a,b) for b > 0.
int fdiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
int fmod_(int a, int b) { return a - fdiv(a, b) * b; }

bool leapGregorianYear(int year) {
  bool l = (year % 4 == 0);
  l = l && !(year % 100 == 0);
  return l || (year % 400 == 0);
}

void setErr(std::string* err, const std::string& what) {
  if (err) *err = what;
}

std::string dateStr(const DateFields& d) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", d.year,
                d.month, d.day, d.hour, d.minute, d.second);
  return buf;
}

// FMS valid_increments: shared field-range validation for every calendar.
bool validRanges(const DateFields& d, std::string* err) {
  if (d.second > 59 || d.second < 0 || d.minute > 59 || d.minute < 0 ||
      d.hour > 23 || d.hour < 0 || d.day > 31 || d.day < 1 || d.month > 12 ||
      d.month < 1 || d.year < 1) {
    setErr(err, "Invalid date. Date=" + dateStr(d));
    return false;
  }
  return true;
}

// ---- per-calendar get_date (span -> date); span is pre-normalized ----

void getDateThirty(const TimeStamp& t, DateFields* d) {
  int rem = t.days;
  const int dyear = rem / (30 * 12);
  d->year = dyear + 1;
  rem -= dyear * (30 * 12);
  const int dmonth = rem / 30;
  d->month = 1 + dmonth;
  d->day = rem - dmonth * 30 + 1;
}

void getDateNoLeap(const TimeStamp& t, DateFields* d) {
  d->year = t.days / 365 + 1;
  int day = t.days % 365 + 1;
  int m = 1;
  for (; m <= 12; ++m) {
    if (day <= kDaysPerMonth[m]) break;
    day -= kDaysPerMonth[m];
  }
  d->month = m;
  d->day = day;
}

void getDateJulian(const TimeStamp& t, DateFields* d) {
  const int nfour = t.days / (4 * 365 + 1);
  int day = t.days % (4 * 365 + 1);
  int nex = day / 365;
  if (nex == 4) {
    nex = 3;
    day = 366;
  } else {
    day = day % 365 + 1;
  }
  const bool leap = (nex == 3);
  d->year = 1 + 4 * nfour + nex;
  int m = 1;
  for (; m <= 12; ++m) {
    int dtm = kDaysPerMonth[m];
    if (leap && m == 2) dtm = 29;
    if (day <= dtm) break;
    day -= dtm;
  }
  d->month = m;
  d->day = day;
}

// Direct port of FMS get_date_gregorian (400-year-cycle arithmetic).
void getDateGregorian(const TimeStamp& t, DateFields* d) {
  const int iday = (t.days + 1) % kDaysIn400Years;

  int yearx = 1, idayx = 0;
  if (iday == 0) {  // day 366 of year 400*k
    yearx = 0;
    idayx = -366;
  } else if (iday > 365) {
    yearx = iday / 365 - 1;  // approximation, off by -1 year at most
    const int ncenturies = yearx / 100;
    const int nlpyrs = (yearx - ncenturies * 100) / 4;
    idayx = ncenturies * 36524 + (yearx - ncenturies * 100) * 365 + nlpyrs;
    if (ncenturies == 4) idayx += 1;  // year 400 is a leap year
    const int l = leapGregorianYear(yearx + 1) ? 1 : 0;
    if (iday - idayx > 365 + l) {
      yearx += 1;
      idayx += 365 + l;
    }
    yearx += 1;
  }
  d->year = 400 * ((t.days + 1) / kDaysIn400Years) + yearx;

  const int l = leapGregorianYear(d->year) ? 1 : 0;
  int dayx = iday - idayx;
  if (dayx <= 31) {
    d->month = 1;
    d->day = dayx;
  } else {
    const int monthx = dayx / 30;
    for (int i = 1; i <= monthx; ++i) {
      dayx -= kDaysPerMonth[i];
      if (i == 2) dayx -= l;
    }
    d->month = monthx + 1;
    d->day = dayx;
    if (dayx <= 0) {
      d->month = monthx;
      d->day = dayx + kDaysPerMonth[monthx];
      if (monthx == 2) d->day += l;
    }
  }
}

// ---- per-calendar set_date (date -> span); ranges pre-validated ----

bool setDateThirty(const DateFields& d, TimeStamp* out, std::string* err) {
  if (d.day > 30) {
    setErr(err, "Invalid date. Date=" + dateStr(d));
    return false;
  }
  out->days = (d.day - 1) + 30 * ((d.month - 1) + 12 * (d.year - 1));
  return true;
}

bool setDateNoLeap(const DateFields& d, TimeStamp* out, std::string* err) {
  if (d.day > kDaysPerMonth[d.month]) {
    setErr(err, "Invalid date. Date=" + dateStr(d));
    return false;
  }
  int ndays = 0;
  for (int m = 1; m < d.month; ++m) ndays += kDaysPerMonth[m];
  out->days = d.day - 1 + ndays + 365 * (d.year - 1);
  return true;
}

bool setDateJulian(const DateFields& d, TimeStamp* out, std::string* err) {
  if (d.month != 2 && d.day > kDaysPerMonth[d.month]) {
    setErr(err, "Invalid date. Date=" + dateStr(d));
    return false;
  }
  const bool leap = (fmod_(d.year, 4) == 0);
  const int nleapyr = (d.year - 1) / 4;
  if (d.month == 2 && (d.day > 29 || (!leap && d.day > 28))) {
    setErr(err, "Invalid date. Date=" + dateStr(d));
    return false;
  }
  int ndays = 0;
  for (int m = 1; m < d.month; ++m) {
    ndays += kDaysPerMonth[m];
    if (leap && m == 2) ndays += 1;
  }
  out->days = d.day - 1 + ndays + 365 * (d.year - nleapyr - 1) + 366 * nleapyr;
  return true;
}

bool setDateGregorian(const DateFields& d, TimeStamp* out, std::string* err) {
  const int l = leapGregorianYear(d.year) ? 1 : 0;
  const int cap = (d.month == 2) ? kDaysPerMonth[2] + l : kDaysPerMonth[d.month];
  if (d.day > cap || d.day < 1) {
    setErr(err, "Invalid date. Date=" + dateStr(d));
    return false;
  }

  const int yearx = fmod_(d.year - 1, 400);
  int dayx = 0;
  if (yearx > 0) {
    const int ncenturies = yearx / 100;
    const int nlpyrs = (yearx - ncenturies * 100) / 4;
    dayx = ncenturies * 36524 + (yearx - ncenturies * 100) * 365 + nlpyrs;
    if (ncenturies == 4) dayx += 1;  // year 400 is a leap year
  }
  static const int kCumDays[13] = {0, 0,   31,  59,  90,  120, 151,
                                   181, 212, 243, 273, 304, 334};
  dayx += kCumDays[d.month] + ((d.month >= 3) ? l : 0);
  out->days = ((d.year - 1) / 400) * kDaysIn400Years + dayx + d.day - 1;
  return true;
}

}  // namespace

bool calendarFromFms(int fms_type, Calendar* out) {
  if (fms_type < 0 || fms_type > 4) return false;
  *out = static_cast<Calendar>(fms_type);
  return true;
}

bool makeTime(int seconds, int days, TimeStamp* out, std::string* err) {
  const int days_new = days + fdiv(seconds, kSecondsPerDay);
  const int seconds_new = fmod_(seconds, kSecondsPerDay);
  if (days_new < 0) {
    setErr(err, "time is negative. days=" + std::to_string(days_new) +
                    " seconds=" + std::to_string(seconds_new));
    return false;
  }
  out->days = days_new;
  out->seconds = seconds_new;
  out->ticks = 0;
  return true;
}

bool incrementTime(const TimeStamp& t, int seconds, int days, TimeStamp* out,
                   std::string* err) {
  return makeTime(t.seconds + seconds, t.days + days, out, err);
}

double spanSeconds(const TimeStamp& from, const TimeStamp& to) {
  return (double)(to.days - from.days) * kSecondsPerDay +
         (double)(to.seconds - from.seconds) + (double)(to.ticks - from.ticks);
}

bool fromDate(Calendar cal, const DateFields& d, TimeStamp* out,
              std::string* err) {
  if (!validRanges(d, err)) return false;
  out->seconds = d.second + 60 * (d.minute + 60 * d.hour);
  out->ticks = 0;
  switch (cal) {
    case Calendar::ThirtyDayMonths: return setDateThirty(d, out, err);
    case Calendar::NoLeap:          return setDateNoLeap(d, out, err);
    case Calendar::Julian:          return setDateJulian(d, out, err);
    case Calendar::Gregorian:       return setDateGregorian(d, out, err);
    case Calendar::NoCalendar:
      setErr(err, "cannot produce a date when the calendar type is NO_CALENDAR");
      return false;
  }
  setErr(err, "invalid calendar type");
  return false;
}

bool toDate(Calendar cal, const TimeStamp& t, DateFields* out,
            std::string* err) {
  switch (cal) {
    case Calendar::ThirtyDayMonths: getDateThirty(t, out); break;
    case Calendar::NoLeap:          getDateNoLeap(t, out); break;
    case Calendar::Julian:          getDateJulian(t, out); break;
    case Calendar::Gregorian:       getDateGregorian(t, out); break;
    case Calendar::NoCalendar:
    default:
      setErr(err, "cannot produce a date when the calendar type is NO_CALENDAR");
      return false;
  }
  const int isec = t.seconds;
  out->hour = isec / 3600;
  out->minute = (isec - 3600 * out->hour) / 60;
  out->second = isec - 3600 * out->hour - 60 * out->minute;
  return true;
}

bool incrementDate(Calendar cal, const TimeStamp& t, int years, int months,
                   int days, int hours, int minutes, int seconds,
                   TimeStamp* out, std::string* err) {
  const bool mode_1 = days != 0 || hours != 0 || minutes != 0 || seconds != 0;
  const bool mode_2 = years != 0 || months != 0;

  if (!mode_1 && !mode_2) {
    *out = t;
    return true;
  }
  if (mode_1 && mode_2) {
    setErr(err, "years and/or months must not be incremented with other time units");
    return false;
  }
  if (mode_1) {
    return incrementTime(t, seconds + 60 * (minutes + 60 * hours), days, out,
                         err);
  }

  DateFields d;
  if (!toDate(cal, t, &d, err)) return false;
  d.month += months;
  d.year += fdiv(d.month - 1, 12);       // FMS: floor((cmonth-1)/12.)
  d.month = fmod_(d.month - 1, 12) + 1;  // FMS: modulo(cmonth-1,12)+1
  d.year += years;
  return fromDate(cal, d, out, err);  // keeps day-of-month; may be invalid
}

int daysInMonth(Calendar cal, const TimeStamp& t) {
  DateFields d;
  if (!toDate(cal, t, &d, nullptr)) return 0;
  if (cal == Calendar::ThirtyDayMonths) return 30;
  int n = kDaysPerMonth[d.month];
  if (d.month == 2 && leapYear(cal, t)) n = 29;
  return n;
}

bool leapYear(Calendar cal, const TimeStamp& t) {
  DateFields d;
  if (!toDate(cal, t, &d, nullptr)) return false;
  if (cal == Calendar::Gregorian) return leapGregorianYear(d.year);
  if (cal == Calendar::Julian) return fmod_(d.year, 4) == 0;
  return false;  // ThirtyDayMonths / NoLeap
}

bool addInterval(Calendar cal, const TimeStamp& t, int n, TimeUnit unit,
                 TimeStamp* out, std::string* err) {
  switch (unit) {
    case TimeUnit::Seconds: return incrementTime(t, n, 0, out, err);
    case TimeUnit::Minutes: return incrementTime(t, 60 * n, 0, out, err);
    case TimeUnit::Hours:   return incrementTime(t, 3600 * n, 0, out, err);
    case TimeUnit::Days:    return incrementTime(t, 0, n, out, err);
    case TimeUnit::Months:
      return incrementDate(cal, t, 0, n, 0, 0, 0, 0, out, err);
    case TimeUnit::Years:
      return incrementDate(cal, t, n, 0, 0, 0, 0, 0, out, err);
  }
  setErr(err, "invalid time unit");
  return false;
}

}  // namespace TIM
