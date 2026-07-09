#include "tim_external_field.hpp"

#include "tim_iosystem.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace TIM {
namespace IO {

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// FMS time_divide: spans as double seconds, divided in double.
double timeDivide(const TimeStamp& num, const TimeStamp& den) {
  const double d1 = (double)num.days * 86400.0 + (double)num.seconds;
  const double d2 = (double)den.days * 86400.0 + (double)den.seconds;
  return d1 / d2;
}

TimeStamp tdiff(const TimeStamp& a, const TimeStamp& b) {  // a - b (a >= b)
  TimeStamp r;
  std::string err;
  makeTime(a.seconds - b.seconds, a.days - b.days, &r, &err);
  return r;
}

TimeStamp tadd(const TimeStamp& a, const TimeStamp& b) {
  TimeStamp r;
  std::string err;
  makeTime(a.seconds + b.seconds, a.days + b.days, &r, &err);
  return r;
}

// FMS (t1+t2)/2: integer floor of the summed span.
TimeStamp halfway(const TimeStamp& a, const TimeStamp& b) {
  const long long tot = ((long long)a.days + b.days) * 86400ll +
                        (long long)a.seconds + b.seconds;
  const long long half = tot / 2;
  return TimeStamp{(int)(half / 86400ll), (int)(half % 86400ll), 0};
}

// get_cal_time units parsing: "<unit> since YYYY-MM-DD[ hh:mm:ss]".
// Returns false on an unparseable string.
bool parseTimeUnits(const std::string& units, Calendar cal, TimeStamp* base,
                    double* to_days, std::string* err) {
  const std::string lu = lower(units);
  double per_day;
  size_t pos;
  if (lu.rfind("day", 0) == 0) per_day = 1.0;
  else if (lu.rfind("hour", 0) == 0) per_day = 24.0;
  else if (lu.rfind("minute", 0) == 0) per_day = 1440.0;
  else if (lu.rfind("second", 0) == 0) per_day = 86400.0;
  else { *err = "unsupported time units '" + units + "'"; return false; }
  *to_days = per_day;

  pos = lu.find("since");
  if (pos == std::string::npos) {
    *err = "no 'since' in time units '" + units + "'";
    return false;
  }
  std::string datestr = units.substr(pos + 5);
  for (auto& c : datestr)
    if (c == '-' || c == ':' || c == 'T') c = ' ';
  std::istringstream is(datestr);
  DateFields d;
  if (!(is >> d.year >> d.month >> d.day)) {
    *err = "cannot parse base date in '" + units + "'";
    return false;
  }
  double hh = 0, mm = 0, ss = 0;
  if (is >> hh) { is >> mm; is >> ss; }
  d.hour = (int)hh; d.minute = (int)mm; d.second = (int)ss;
  return fromDate(cal, d, base, err);
}

// get_cal_time value conversion: floor/truncate split (days-equivalent).
TimeStamp calTime(double value, double per_day, const TimeStamp& base) {
  const double vdays = value / per_day;
  const int days = (int)std::floor(vdays);
  const int secs = (int)(86400.0 * (vdays - days));
  TimeStamp r;
  std::string err;
  makeTime(base.seconds + secs, base.days + days, &r, &err);
  return r;
}

bool calendarMatches(const std::string& att, Calendar cal) {
  const std::string a = lower(att);
  switch (cal) {
    case Calendar::NoLeap: return a == "noleap" || a == "365_day";
    case Calendar::Julian: return a == "julian";
    case Calendar::Gregorian: return a == "gregorian" || a == "standard";
    case Calendar::ThirtyDayMonths: return a == "thirty_day_months" || a == "360_day";
    case Calendar::NoCalendar: default: return a == "no_calendar";
  }
}

// FMS bisect: Timelist(i1) <= T <= Timelist(i2), i2 = i1+1.
void bisect(const std::vector<TimeStamp>& list, int n, const TimeStamp& t,
            int* i1, int* i2) {
  if (t == list[0]) { *i1 = 1; *i2 = 2; return; }
  if (t == list[(size_t)n - 1]) { *i1 = n; *i2 = n + 1; return; }
  int il = 0, iu = n + 1;
  while (iu - il > 1) {
    const int i = (iu + il) / 2;
    if (list[(size_t)i - 1] > t) iu = i; else il = i;
  }
  *i1 = il;
  *i2 = il + 1;
}

}  // namespace

ExternalField::ExternalField(IoSystem& sys, const std::string& path,
                             const std::string& field, int domain_key,
                             const Decomp2D& domain, Calendar cal)
    : sys_(sys), path_(path), domain_key_(domain_key), domain_(domain),
      cal_(cal) {
  using S = Backend::Serial;

  // Case-insensitive field resolution (FMS1 compatibility, as the seam does).
  if (S::findVarCI(path_, field, &varname_) != 0) {
    error_ = "field " + field + " not found in " + path_;
    return;
  }

  int sizes[4] = {1, 1, 1, 1};
  const int nd = S::varSizes(path_, varname_, sizes);
  if (nd < 3 || nd > 4) {
    error_ = varname_ + " in " + path_ + " must be (x,y[,z],time)";
    return;
  }
  siz_[0] = sizes[0];
  siz_[1] = sizes[1];
  siz_[2] = (nd == 4) ? sizes[2] : 1;
  siz_[3] = sizes[nd - 1];
  nz_ = siz_[2];

  decomposed_ = domain_key_ >= 0;
  if (decomposed_) {
    if (siz_[0] != domain_.nig() || siz_[1] != domain_.njg()) {
      error_ = varname_ + " in " + path_ + " is not on the model grid";
      return;
    }
    npts_ = domain_.window(Stagger::Center).npts() * (long long)nz_;
  } else {
    npts_ = (long long)siz_[0] * siz_[1] * nz_;
  }

  // Missing value: _FillValue > missing_value > missing > netCDF fill.
  double att;
  if (S::attDouble(path_, varname_, "_FillValue", &att) == 0) missing_ = att;
  else if (S::attDouble(path_, varname_, "missing_value", &att) == 0) missing_ = att;
  else if (S::attDouble(path_, varname_, "missing", &att) == 0) missing_ = att;
  else missing_ = 9.9692099683868690e+36;

  // Valid_t (fms2_io get_valid): scale/offset, ranges, fill-derived range.
  double scale = 1.0, offset = 0.0, rng[2];
  S::attDouble(path_, varname_, "scale_factor", &scale);
  S::attDouble(path_, varname_, "add_offset", &offset);
  if (S::attDouble(path_, varname_, "valid_range", rng, 2) == 0) {
    min_val_ = rng[0] * scale + offset; has_min_ = true;
    max_val_ = rng[1] * scale + offset; has_max_ = true;
  } else {
    if (S::attDouble(path_, varname_, "valid_max", &att) == 0) {
      max_val_ = att * scale + offset; has_max_ = true;
    }
    if (S::attDouble(path_, varname_, "valid_min", &att) == 0) {
      min_val_ = att * scale + offset; has_min_ = true;
    }
  }
  if (S::attDouble(path_, varname_, "missing_value", &att) == 0) {
    missing_val_ = att * scale + offset; has_missing_ = true;
  }
  if (S::attDouble(path_, varname_, "_FillValue", &att) == 0) {
    fill_val_ = att * scale + offset; has_fill_ = true;
    if (!has_min_ && !has_max_) {
      // Two ULPs inside the fill (float/double branch of get_valid).
      const double huge = 1.0e308;
      if (att > 0) {
        max_val_ = std::nextafter(std::nextafter(att, -huge), -huge) * scale + offset;
        has_max_ = true;
      } else {
        min_val_ = std::nextafter(std::nextafter(att, huge), huge) * scale + offset;
        has_min_ = true;
      }
    }
  }

  // Time axis: unlimited coordinate values + get_cal_time conversion.
  if (S::timeName(path_, &timename_) != 0) {
    error_ = "no unlimited time dimension in " + path_;
    return;
  }
  std::vector<double> tvals((size_t)siz_[3]);
  if (S::timeValues(path_, tvals.data(), siz_[3]) != 0) {
    error_ = "cannot read time values from " + path_;
    return;
  }
  std::string units, calatt;
  if (S::attText(path_, timename_, "units", &units) != 0) {
    error_ = "time axis in " + path_ + " has no units attribute";
    return;
  }
  if (S::attText(path_, timename_, "calendar", &calatt) == 0 ||
      S::attText(path_, timename_, "calendar_type", &calatt) == 0) {
    if (!calendarMatches(calatt, cal_)) {
      error_ = "calendar '" + calatt + "' in " + path_ +
               " does not match the model calendar (conversion unsupported)";
      return;
    }
  }
  TimeStamp base;
  double per_day = 1.0;
  if (!parseTimeUnits(units, cal_, &base, &per_day, &error_)) return;
  times_.resize(tvals.size());
  for (size_t k = 0; k < tvals.size(); ++k)
    times_[k] = calTime(tvals[k], per_day, base);

  std::string modulo;
  modulo_ = (S::attText(path_, timename_, "modulo", &modulo) == 0);
  if (modulo_) {
    // set_time_modulo: rewrite every record's year to modulo_year (= 1).
    for (auto& t : times_) {
      DateFields d;
      std::string err;
      if (!toDate(cal_, t, &d, &err)) { error_ = err; return; }
      d.year = 1;
      if (!fromDate(cal_, d, &t, &err)) { error_ = err; return; }
    }
  }

  if (decomposed_) {
    file_ = File::openForRead(sys_, path_);
    if (!file_) {
      error_ = "cannot open " + path_ + " for decomposed reads";
      return;
    }
  }
  buf_[0].resize((size_t)npts_);
  buf_[1].resize((size_t)npts_);
  ok_ = true;
}

bool ExternalField::valid(double x) const {
  bool v = true;
  if (has_min_ || has_max_) {
    if (has_min_ && !has_max_) v = x >= min_val_;
    else if (has_max_ && !has_min_) v = x <= max_val_;
    else v = !(x < min_val_ || x > max_val_);
  }
  if (has_fill_ || has_missing_) {
    if (has_fill_ && !has_missing_) v = v && (x != fill_val_);
    else if (has_missing_ && !has_fill_) v = v && (x != missing_val_);
    else v = v && !(x == missing_val_ || x == fill_val_);
  }
  return v;
}

int ExternalField::loadRecord(int rec, int* slot, int avoid) {
  for (int s = 0; s < 2; ++s)
    if (buf_rec_[s] == rec) { *slot = s; return 0; }
  int s = next_slot_;
  if (s == avoid) s = 1 - s;  // never evict the partner record
  next_slot_ = 1 - s;
  int rc;
  if (decomposed_) {
    rc = file_->readDecomposed(varname_, domain_key_, domain_,
                               Stagger::Center, rec, nz_, 1, buf_[s].data());
  } else {
    // Replicated read of one record: full-grid hyperslab (readPlain is
    // 0d/1d-only). start/count are 1-based Fortran order (x,y,z,t).
    int start[4] = {1, 1, 1, 1}, count[4] = {siz_[0], siz_[1], 1, 1};
    if (siz_[2] > 1) {  // (x,y,z,t)
      count[2] = siz_[2];
      start[3] = rec;
    } else {            // (x,y,t)
      start[2] = rec;
    }
    rc = Backend::Serial::readSlab(path_, varname_, start, count,
                                   buf_[s].data());
  }
  if (rc != 0) {
    error_ = "record read failed for " + varname_ + " in " + path_;
    return rc;
  }
  buf_rec_[s] = rec;
  *slot = s;
  return 0;
}

// FMS time_interp_list (modtime = YEAR for modulo axes, NONE otherwise);
// rec1/rec2 are 1-based record numbers.
int ExternalField::bracket(const TimeStamp& t_in, double* w2, int* rec1,
                           int* rec2) {
  std::string err;
  int n = (int)times_.size();
  TimeStamp T = t_in;
  TimeStamp period{0, 0, 0};

  if (modulo_) {
    const TimeStamp tmod = halfway(times_[0], times_[(size_t)n - 1]);
    DateFields dm;
    if (!toDate(cal_, tmod, &dm, &err)) { error_ = err; return -1; }
    const bool mod_leap = leapYear(cal_, tmod);
    // Period = days_in_year(Time_mod)
    TimeStamp y0, y1;
    DateFields jan1{dm.year, 1, 1, 0, 0, 0}, jan1n{dm.year + 1, 1, 1, 0, 0, 0};
    if (!fromDate(cal_, jan1, &y0, &err) || !fromDate(cal_, jan1n, &y1, &err)) {
      error_ = err;
      return -1;
    }
    period = tdiff(y1, y0);
    // Timelist spanning exactly one period: ignore the last record.
    if (tdiff(times_[(size_t)n - 1], times_[0]) == period) n = n - 1;
    if (tdiff(times_[(size_t)n - 1], times_[0]) > period) {
      error_ = "period of time list exceeds modulo period in " + path_;
      return -1;
    }
    // set_modtime(Time, YEAR)
    DateFields d;
    if (!toDate(cal_, t_in, &d, &err)) { error_ = err; return -1; }
    d.year = dm.year;
    if (!mod_leap && d.month == 2 && d.day > 28) {
      d.month = 3;
      d.day -= 28;
    }
    if (!fromDate(cal_, d, &T, &err)) { error_ = err; return -1; }
  }

  const TimeStamp Ts = times_[0], Te = times_[(size_t)n - 1];

  if (T >= Ts && T < Te) {
    int i1, i2;
    bisect(times_, n, T, &i1, &i2);
    *rec1 = i1;
    *rec2 = i2;
    *w2 = timeDivide(tdiff(T, times_[(size_t)i1 - 1]),
                     tdiff(times_[(size_t)i2 - 1], times_[(size_t)i1 - 1]));
  } else if (T < Ts) {
    if (!modulo_) {
      error_ = "model time is before the time list in " + path_;
      return -1;
    }
    const TimeStamp Td = tdiff(Te, Ts);
    *w2 = 1.0 - timeDivide(tdiff(Ts, T), tdiff(period, Td));
    *rec1 = n;
    *rec2 = 1;
  } else if (T == Te) {
    *w2 = 0.0;
    *rec1 = n;
    *rec2 = modulo_ ? 1 : n;
  } else {  // T > Te
    if (!modulo_) {
      error_ = "model time is after the time list in " + path_;
      return -1;
    }
    const TimeStamp Td = tdiff(Te, Ts);
    *w2 = timeDivide(tdiff(T, Te), tdiff(period, Td));
    *rec1 = n;
    *rec2 = 1;
  }
  return 0;
}

int ExternalField::interp(const TimeStamp& t, double* out,
                          unsigned char* mask) {
  if (!ok_) return -1;

  if (siz_[3] == 1) {  // time-independent field
    int s;
    if (loadRecord(1, &s) != 0) return -1;
    const double* b = buf_[s].data();
    for (long long i = 0; i < npts_; ++i) {
      const bool m = valid(b[i]);
      out[i] = m ? b[i] : missing_;
      if (mask) mask[i] = m ? 1 : 0;
    }
    return 0;
  }

  double w2;
  int r1, r2;
  if (bracket(t, &w2, &r1, &r2) != 0) return -1;
  const double w1 = 1.0 - w2;

  int s1, s2;
  if (loadRecord(r1, &s1) != 0) return -1;
  if (loadRecord(r2, &s2, s1) != 0) return -1;
  const double* b1 = buf_[s1].data();
  const double* b2 = buf_[s2].data();

  for (long long i = 0; i < npts_; ++i) {
    const bool m = valid(b1[i]) && valid(b2[i]);
    out[i] = m ? b1[i] * w1 + b2[i] * w2 : missing_;
    if (mask) mask[i] = m ? 1 : 0;
  }
  return 0;
}

}  // namespace IO
}  // namespace TIM
