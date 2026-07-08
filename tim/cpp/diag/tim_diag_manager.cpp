#include "tim_diag_manager.hpp"

#include "../io/tim_iosystem.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace TIM {
namespace Diag {

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string fieldKey(const std::string& module, const std::string& field) {
  return lower(module) + "|" + lower(field);
}

const char* calendarName(Calendar c) {
  switch (c) {
    case Calendar::ThirtyDayMonths: return "thirty_day_months";
    case Calendar::Julian: return "julian";
    case Calendar::Gregorian: return "gregorian";
    case Calendar::NoLeap: return "noleap";
    case Calendar::NoCalendar: default: return "no_calendar";
  }
}

const char* unitName(TimeUnit u) {
  switch (u) {
    case TimeUnit::Seconds: return "seconds";
    case TimeUnit::Minutes: return "minutes";
    case TimeUnit::Hours: return "hours";
    case TimeUnit::Days: return "days";
    case TimeUnit::Months: return "months";
    case TimeUnit::Years: default: return "years";
  }
}

double unitSeconds(TimeUnit u) {
  switch (u) {
    case TimeUnit::Seconds: return 1.0;
    case TimeUnit::Minutes: return 60.0;
    case TimeUnit::Hours: return 3600.0;
    case TimeUnit::Days: default: return 86400.0;
  }
}

// FMS (t1+t2)/2: exact halving of the summed span, floor to whole seconds.
TimeStamp halfway(const TimeStamp& a, const TimeStamp& b) {
  const long long tot = ((long long)a.days + b.days) * 86400ll +
                        (long long)a.seconds + b.seconds;
  const long long half = tot / 2;
  return TimeStamp{(int)(half / 86400ll), (int)(half % 86400ll), 0};
}

// The FMS "time: X" cell_methods suffix per reduction.
const char* timeMethod(Reduction r) {
  switch (r) {
    case Reduction::None: return "point";
    case Reduction::Mean: case Reduction::Diurnal: return "mean";
    case Reduction::Min: return "min";
    case Reduction::Max: return "max";
    case Reduction::RMS: return "root_mean_square";
    case Reduction::Pow: default: return "mean";
  }
}

bool isAveraged(Reduction r) {
  return r == Reduction::Mean || r == Reduction::RMS || r == Reduction::Pow;
}

constexpr TimeStamp kFarFuture{INT_MAX / 4, 0, 0};

}  // namespace

// ---------------------------------------------------------------------------
// FMS get_time_string port (diag_util.F90): fixed token order yr,mo,dy,hr,
// mi,sc; each token "-<zero-padded value>"; absent coarser units cascade into
// the next finer one; the leading '-' becomes '.'.

std::string fmsBaseName(const std::string& name, bool* has_percent) {
  size_t pos = std::string::npos;
  for (size_t i = 0; i + 1 < name.size(); ++i) {
    if (name[i] == '%' && std::isdigit((unsigned char)name[i + 1])) {
      pos = i;
      break;
    }
  }
  if (has_percent) *has_percent = pos != std::string::npos;
  return pos == std::string::npos ? name : name.substr(0, pos);
}

std::string fmsTimeSuffix(const std::string& name, Calendar cal,
                          const TimeStamp& t) {
  bool has_percent = false;
  fmsBaseName(name, &has_percent);
  if (!has_percent) return "";
  const std::string tail = name.substr(fmsBaseName(name).size());

  DateFields d;
  std::string err;
  if (!toDate(cal, t, &d, &err)) {  // NO_CALENDAR: day/second based tokens
    d = DateFields{0, 0, 0, t.seconds / 3600, (t.seconds / 60) % 60,
                   t.seconds % 60};
  }

  auto token = [&](const char* key, int value, std::string* out) -> bool {
    const size_t p = tail.find(key);
    if (p == std::string::npos || p == 0) { out->clear(); return false; }
    const int width = tail[p - 1] - '0';
    char buf[32];
    std::snprintf(buf, sizeof buf, "-%0*d", width > 0 ? width : 1, value);
    *out = buf;
    return true;
  };

  static const int kDpm[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  std::string yr, mo, dy, hr, mi, sc;
  int yr2 = 0, dy2 = 0, hr2 = 0, mi2 = 0;

  if (token("yr", d.year, &yr)) yr2 = 0; else yr2 = d.year - 1;
  token("mo", yr2 * 12 + d.month, &mo);

  int dy1_s;
  if (!mo.empty()) {
    dy1_s = d.day;
    dy2 = dy1_s - 1;
  } else if (!yr.empty()) {
    int julian_day = d.day;
    for (int i = 1; i < d.month; ++i) julian_day += kDpm[i];
    if (leapYear(cal, t) && d.month > 2) julian_day += 1;
    dy1_s = julian_day;
    dy2 = dy1_s - 1;
  } else {
    dy1_s = t.days;  // absolute day count (FMS get_time)
    dy2 = dy1_s;
  }
  token("dy", dy1_s, &dy);

  const int hr1_s = !dy.empty() ? d.hour : dy2 * 24 + d.hour;
  hr2 = hr1_s;
  token("hr", hr1_s, &hr);

  const int mi1_s = !hr.empty() ? d.minute : hr2 * 60 + d.minute;
  mi2 = mi1_s;
  token("mi", mi1_s, &mi);

  const int sc1_s = !mi.empty() ? d.second : mi2 * 60 + d.second;
  token("sc", sc1_s, &sc);

  std::string s = yr + mo + dy + hr + mi + sc;
  if (!s.empty()) s[0] = '.';
  return s;
}

// ---------------------------------------------------------------------------

DiagManager::DiagManager(const DiagConfig& config, Calendar cal,
                         const TimeStamp& init_time, IO::IoSystem& sys,
                         const Options& opts)
    : cfg_(config), cal_(cal), init_time_(init_time), sys_(sys), opts_(opts) {
  std::string err;
  if (!fromDate(cal_, cfg_.base_date, &base_time_, &err)) {
    error_ = "diag_table base date: " + err;
    return;
  }
  if (base_time_ > init_time_) {
    error_ = "diag_table base date is later than the model start time";
    return;
  }
  time_end_ = kFarFuture;

  files_.reserve(cfg_.files.size());
  for (const auto& fs : cfg_.files) {
    OutFile f;
    f.spec = &fs;
    if (fs.time_axis_units == TimeUnit::Months ||
        fs.time_axis_units == TimeUnit::Years) {
      error_ = "file '" + fs.name + "': months/years time axis not supported";
      return;
    }
    if (cal_ == Calendar::NoCalendar &&
        (fs.output_freq_units == TimeUnit::Months ||
         fs.output_freq_units == TimeUnit::Years ||
         fs.new_file_freq_units == TimeUnit::Months ||
         fs.new_file_freq_units == TimeUnit::Years)) {
      error_ = "file '" + fs.name + "': month/year frequencies need a calendar";
      return;
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s since %04d-%02d-%02d %02d:%02d:%02d",
                  unitName(fs.time_axis_units), cfg_.base_date.year,
                  cfg_.base_date.month, cfg_.base_date.day,
                  cfg_.base_date.hour, cfg_.base_date.minute,
                  cfg_.base_date.second);
    f.time_units_str = buf;
    f.unit_seconds = unitSeconds(fs.time_axis_units);

    // Rollover trio (FMS init_file + sync_file_times): start at the table's
    // start_time (else base date), duration defaulting to new_file_freq,
    // then advance whole periods until the file covers the run.
    f.rollover = fs.new_file_freq > 0;
    f.start_time = base_time_;
    if (!fs.start_time.empty()) {
      DateFields sd;
      std::istringstream is(fs.start_time);
      if (is >> sd.year >> sd.month >> sd.day >> sd.hour >> sd.minute >>
          sd.second) {
        if (!fromDate(cal_, sd, &f.start_time, &err)) {
          error_ = "file '" + fs.name + "' start_time: " + err;
          return;
        }
      }
    }
    if (f.rollover) {
      bool has_percent = false;
      fmsBaseName(fs.name, &has_percent);
      if (!has_percent) {
        error_ = "file '" + fs.name +
                 "' has new_file_freq but no %-token for the time stamp";
        return;
      }
      const int dur = fs.file_duration > 0 ? fs.file_duration : fs.new_file_freq;
      const TimeUnit dur_u =
          fs.file_duration > 0 ? fs.file_duration_units : fs.new_file_freq_units;
      auto advance = [&](std::string* aerr) -> bool {
        if (!addInterval(cal_, f.start_time, fs.new_file_freq,
                         fs.new_file_freq_units, &f.next_open, aerr))
          return false;
        return addInterval(cal_, f.start_time, dur, dur_u, &f.close_time, aerr);
      };
      if (!advance(&err)) { error_ = "file '" + fs.name + "': " + err; return; }
      if (f.next_open < f.close_time) {
        error_ = "file '" + fs.name + "': close time after next_open";
        return;
      }
      while (f.close_time <= init_time_) {
        f.start_time = f.next_open;
        if (!advance(&err)) { error_ = "file '" + fs.name + "': " + err; return; }
      }
    } else {
      f.next_open = kFarFuture;
      f.close_time = kFarFuture;
    }
    files_.push_back(std::move(f));
  }
  ok_ = true;
}

DiagManager::~DiagManager() = default;

int DiagManager::streamWindowsInit(Stream& s) {
  const FileSpec& fs = *files_[(size_t)s.outfile].spec;
  std::string err;
  if (fs.output_freq < 0) {  // END_OF_RUN
    s.last_output = init_time_;
    s.next_output = kFarFuture;
    s.next_next_output = kFarFuture;
    return 0;
  }
  if (fs.output_freq == 0) {  // EVERY_TIME
    s.last_output = init_time_;
    s.next_output = init_time_;
    s.next_next_output = init_time_;
    return 0;
  }
  // Anchor at the base date and walk past init_time (deliberate divergence
  // from FMS's init_time anchoring; identical whenever the run starts on a
  // window boundary, and the property that makes restart-spanning windows
  // well-defined).
  TimeStamp next = base_time_, last = base_time_;
  while (next <= init_time_) {
    last = next;
    if (!addInterval(cal_, next, fs.output_freq, fs.output_freq_units, &next,
                     &err)) {
      error_ = "window walk for '" + fs.name + "': " + err;
      return -1;
    }
  }
  s.last_output = last;
  s.next_output = next;
  if (!addInterval(cal_, next, fs.output_freq, fs.output_freq_units,
                   &s.next_next_output, &err)) {
    error_ = "window walk for '" + fs.name + "': " + err;
    return -1;
  }
  return 0;
}

int DiagManager::registerField(const std::string& module,
                               const std::string& field,
                               const std::vector<int>& axis_ids,
                               const FieldOptions& opts) {
  const std::string key = fieldKey(module, field);
  auto it = field_ids_.find(key);
  if (it != field_ids_.end()) return it->second;

  // Table entries requesting this field (case-insensitive, as FMS).
  std::vector<const FieldSpec*> specs;
  for (const auto& fs : cfg_.fields) {
    if (lower(fs.module_name) == lower(module) &&
        lower(fs.field_name) == lower(field) && fs.file_name != "null")
      specs.push_back(&fs);
  }
  if (specs.empty()) return kFieldNotFound;

  FieldRec fr;
  fr.module = module;
  fr.name = field;
  fr.opts = opts;
  bool sx = false, sy = false;
  std::vector<int> zlens;
  for (int id : axis_ids) {
    if (id == kNullAxis) continue;
    if (!axes_.valid(id)) {
      std::fprintf(stderr, "TIM diag: %s/%s has invalid axis id %d\n",
                   module.c_str(), field.c_str(), id);
      return kFieldNotFound;
    }
    fr.axes.push_back(id);
    const AxisSpec& a = axes_.at(id);
    if (a.domain_key >= 0) {
      fr.decomposed = true;
      fr.domain_key = a.domain_key;
      fr.domain = a.domain;
      if (a.cartesian == "X" && a.staggered) sx = true;
      if (a.cartesian == "Y" && a.staggered) sy = true;
    } else {
      zlens.push_back((int)a.values.size());
    }
  }
  fr.stagger = sx && sy ? Stagger::Corner
               : sx    ? Stagger::EastFace
               : sy    ? Stagger::NorthFace
                       : Stagger::Center;
  for (int zl : zlens) fr.nz *= zl;
  fr.nz2 = (zlens.size() > 1) ? zlens.back() : 1;
  fr.npts = fr.decomposed ? fr.domain.window(fr.stagger).npts() * fr.nz
                          : (long long)fr.nz;

  const int fid = (int)fields_.size();
  for (const FieldSpec* spec : specs) {
    if (spec->reduction == Reduction::Diurnal) {
      std::fprintf(stderr,
                   "TIM diag: diurnal output not supported in prototype "
                   "(%s/%s -> %s), skipping stream\n",
                   module.c_str(), field.c_str(), spec->file_name.c_str());
      continue;
    }
    int fidx = -1;
    for (size_t k = 0; k < files_.size(); ++k)
      if (files_[k].spec->name == spec->file_name) { fidx = (int)k; break; }
    if (fidx < 0) continue;  // parser validates; belt and braces
    OutFile& of = files_[(size_t)fidx];

    if (fr.decomposed) {
      if (!of.has_domain) {
        of.has_domain = true;
        of.domain_key = fr.domain_key;
        of.domain = fr.domain;
      } else if (of.domain_key != fr.domain_key) {
        std::fprintf(stderr,
                     "TIM diag: file %s mixes domains (%d vs %d); "
                     "skipping %s/%s\n",
                     spec->file_name.c_str(), of.domain_key, fr.domain_key,
                     module.c_str(), field.c_str());
        continue;
      }
    }

    Stream s;
    s.field = fid;
    s.outfile = fidx;
    s.spec = spec;
    s.time_ops = isAveraged(spec->reduction) && !opts.is_static;
    s.acc = std::make_unique<Accumulator>(
        fr.npts, opts.is_static ? Reduction::None : spec->reduction,
        spec->pow_exponent, opts.missing_value, opts.mask_variant);
    if (streamWindowsInit(s) != 0) return kFieldNotFound;
    of.time_ops = of.time_ops || s.time_ops;
    of.streams.push_back((int)streams_.size());
    fr.streams.push_back((int)streams_.size());
    streams_.push_back(std::move(s));
  }
  if (fr.streams.empty()) return kFieldNotFound;

  fields_.push_back(std::move(fr));
  field_ids_[key] = fid;
  return fid;
}

int DiagManager::fieldId(const std::string& module,
                         const std::string& field) const {
  auto it = field_ids_.find(fieldKey(module, field));
  return it == field_ids_.end() ? kFieldNotFound : it->second;
}

void DiagManager::addAttribute(int field_id, const std::string& name,
                               const std::string& text) {
  if (field_id < 0 || field_id >= (int)fields_.size()) return;
  for (auto& a : fields_[(size_t)field_id].attrs) {
    if (a.name == name && a.kind == Attr::Kind::Text) {
      a.text += " " + text;  // FMS prepend_attribute: repeats append
      return;
    }
  }
  fields_[(size_t)field_id].attrs.push_back(
      Attr{Attr::Kind::Text, name, text, {}, {}});
}

void DiagManager::addAttribute(int field_id, const std::string& name,
                               const int* v, int n) {
  if (field_id < 0 || field_id >= (int)fields_.size()) return;
  Attr a{Attr::Kind::Ints, name, "", std::vector<int>(v, v + n), {}};
  fields_[(size_t)field_id].attrs.push_back(std::move(a));
}

void DiagManager::addAttribute(int field_id, const std::string& name,
                               const double* v, int n) {
  if (field_id < 0 || field_id >= (int)fields_.size()) return;
  Attr a{Attr::Kind::Reals, name, "", {}, std::vector<double>(v, v + n)};
  fields_[(size_t)field_id].attrs.push_back(std::move(a));
}

double DiagManager::toAxisUnits(const OutFile& f, const TimeStamp& t) const {
  return spanSeconds(base_time_, t) / f.unit_seconds;
}

std::string DiagManager::filePath(const OutFile& f,
                                  const TimeStamp& fname_time) const {
  std::string name = fmsBaseName(f.spec->name);
  if (f.rollover) name += fmsTimeSuffix(f.spec->name, cal_, fname_time);
  if (opts_.prepend_date) {
    DateFields d;
    std::string err;
    if (toDate(cal_, init_time_, &d, &err)) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%04d%02d%02d.", d.year, d.month, d.day);
      name = buf + name;
    }
  }
  if (name.size() < 3 || name.compare(name.size() - 3, 3, ".nc") != 0)
    name += ".nc";
  return name;
}

int DiagManager::ensureOpen(OutFile& f, const TimeStamp& fname_time,
                            std::string* err) {
  if (f.file) return 0;
  const std::string path = filePath(f, fname_time);
  auto file = IO::File::create(sys_, path, f.has_domain ? f.domain_key : -1,
                               f.domain, IO::File::Mode::Overwrite);
  if (!file) {
    if (err) *err = "cannot create " + path;
    return -1;
  }
  f.file = std::move(*file);
  return defineFileContents(f);
}

int DiagManager::defineFileContents(OutFile& f) {
  IO::File& file = *f.file;
  file.putGlobalAtt("title", opts_.title.empty() ? cfg_.title : opts_.title);
  file.putGlobalAtt("grid_type", "regular");
  file.putGlobalAtt("grid_tile", "N/A");

  // Axes used by this file's streams, in first-appearance order, with the
  // edges closure (an axis's edges axis is written too, as FMS does).
  std::vector<int> used;
  auto addAxis = [&](int id) {
    if (std::find(used.begin(), used.end(), id) == used.end()) {
      used.push_back(id);
      const AxisSpec& a = axes_.at(id);
      if (a.edges != kNullAxis &&
          std::find(used.begin(), used.end(), a.edges) == used.end())
        used.push_back(a.edges);
    }
  };
  for (int si : f.streams)
    for (int id : fields_[(size_t)streams_[(size_t)si].field].axes) addAxis(id);

  for (int id : used) {
    const AxisSpec& a = axes_.at(id);
    IO::File::AxisKind kind = IO::File::AxisKind::Fixed;
    Stagger pos = Stagger::Center;
    if (a.domain_key >= 0 && a.cartesian == "X") {
      kind = IO::File::AxisKind::X;
      pos = a.staggered ? Stagger::EastFace : Stagger::Center;
    } else if (a.domain_key >= 0 && a.cartesian == "Y") {
      kind = IO::File::AxisKind::Y;
      pos = a.staggered ? Stagger::NorthFace : Stagger::Center;
    }
    const std::string units = (a.units == "none") ? "" : a.units;
    file.defineAxis(a.name, kind, pos, (int)a.values.size(), units,
                    a.long_name.empty() ? a.name : a.long_name, "",
                    std::nullopt);
    if (!a.cartesian.empty() && a.cartesian != "N")
      file.putVarAtt(a.name, "axis", a.cartesian);
    if (a.direction != 0)
      file.putVarAtt(a.name, "positive", a.direction > 0 ? "up" : "down");
  }

  // Time axis + (for averaged files) the bounds machinery.
  const std::string& tname = f.spec->time_axis_name;
  file.defineAxis(tname, IO::File::AxisKind::Time, Stagger::Center, 0,
                  f.time_units_str, tname, "", std::nullopt);
  file.putVarAtt(tname, "axis", "T");
  file.putVarAtt(tname, "calendar", calendarName(cal_));
  if (f.time_ops) {
    file.putVarAtt(tname, "bounds", "time_bounds");
    file.defineAxis("nbnd", IO::File::AxisKind::Fixed, Stagger::Center, 2, "",
                    "bounds", "", std::nullopt);
    file.defineVar("average_T1", {tname}, f.time_units_str,
                   "Start time for average period", "", false, "",
                   kCmorMissing);
    file.defineVar("average_T2", {tname}, f.time_units_str,
                   "End time for average period", "", false, "", kCmorMissing);
    file.defineVar("average_DT", {tname},
                   unitName(f.spec->time_axis_units),
                   "Length of average period", "", false, "", kCmorMissing);
    file.defineVar("time_bounds", {"nbnd", tname}, f.time_units_str,
                   "time interval endpoints", "", false, "");
    file.putVarAtt("time_bounds", "calendar", calendarName(cal_));
  }

  // Data variables.
  for (int si : f.streams) {
    const Stream& s = streams_[(size_t)si];
    const FieldRec& fr = fields_[(size_t)s.field];
    std::vector<std::string> dims;
    for (int id : fr.axes) dims.push_back(axes_.at(id).name);
    if (!fr.opts.is_static) dims.push_back(tname);
    const bool single = s.spec->packing == 2;
    const double miss = fr.opts.missing_value.value_or(kCmorMissing);
    file.defineVar(s.spec->output_name, dims, fr.opts.units,
                   fr.opts.long_name.empty() ? fr.name : fr.opts.long_name,
                   fr.opts.standard_name, single, "", miss);
    const std::string& vn = s.spec->output_name;
    if (!fr.opts.interp_method.empty())
      file.putVarAtt(vn, "interp_method", fr.opts.interp_method);
    if (fr.opts.valid_range) {
      const double r[2] = {fr.opts.valid_range->first,
                           fr.opts.valid_range->second};
      file.putVarAtt(vn, "valid_range", r, 2, single);
    }
    std::string cell_measures;
    auto measureName = [&](int mid) -> std::string {
      if (mid < 0 || mid >= (int)fields_.size()) return "";
      const FieldRec& mf = fields_[(size_t)mid];
      return mf.streams.empty() ? mf.name
                                : streams_[(size_t)mf.streams[0]].spec->output_name;
    };
    if (fr.opts.area_field != kFieldNotFound)
      cell_measures += "area: " + measureName(fr.opts.area_field);
    if (fr.opts.volume_field != kFieldNotFound)
      cell_measures += (cell_measures.empty() ? "" : " ") +
                       std::string("volume: ") +
                       measureName(fr.opts.volume_field);
    if (!cell_measures.empty())
      file.putVarAtt(vn, "cell_measures", cell_measures);

    std::string cell_methods;
    for (const Attr& a : fr.attrs) {
      if (a.name == "cell_methods" && a.kind == Attr::Kind::Text) {
        cell_methods = a.text;
        continue;
      }
      switch (a.kind) {
        case Attr::Kind::Text: file.putVarAtt(vn, a.name, a.text); break;
        case Attr::Kind::Ints:
          file.putVarAttInts(vn, a.name, a.ints.data(), (int)a.ints.size());
          break;
        case Attr::Kind::Reals:
          file.putVarAtt(vn, a.name, a.reals.data(), (int)a.reals.size(),
                         single);
          break;
      }
    }
    if (!fr.opts.is_static) {
      cell_methods += (cell_methods.empty() ? "" : " ") +
                      std::string("time: ") + timeMethod(s.spec->reduction);
    }
    if (!cell_methods.empty()) file.putVarAtt(vn, "cell_methods", cell_methods);
    if (s.time_ops)
      file.putVarAtt(vn, "time_avg_info", "average_T1,average_T2,average_DT");
  }

  // Coordinate values (first writeAxis flips out of define mode).
  for (int id : used) {
    const AxisSpec& a = axes_.at(id);
    file.writeAxis(a.name, a.values.data(), (int)a.values.size());
  }
  if (f.time_ops) {
    static const double bnds[2] = {1.0, 2.0};
    file.writeAxis("nbnd", bnds, 2);
  }
  return 0;
}

int DiagManager::writeWindow(Stream& s, bool at_end, const TimeStamp& end_time,
                             std::string* err) {
  OutFile& f = files_[(size_t)s.outfile];
  const FieldRec& fr = fields_[(size_t)s.field];
  const int freq = f.spec->output_freq;

  if (at_end && freq < 0) s.next_output = end_time;  // END_OF_RUN closes here

  const TimeStamp mid = halfway(s.last_output, s.next_output);
  const TimeStamp rec_time = s.time_ops ? mid : s.next_output;
  // filename_time_bounds: classic column absent -> FMS default "middle".
  const TimeStamp fname_time = s.time_ops ? mid : s.next_output;

  // Rollover state machine (FMS trio). Data in (close_time, next_open] gap
  // is dropped.
  bool drop = false;
  if (f.rollover) {
    bool rolled = false;
    std::string aerr;
    while (rec_time > f.next_open) {
      f.start_time = f.next_open;
      const int dur =
          f.spec->file_duration > 0 ? f.spec->file_duration : f.spec->new_file_freq;
      const TimeUnit dur_u = f.spec->file_duration > 0
                                 ? f.spec->file_duration_units
                                 : f.spec->new_file_freq_units;
      if (!addInterval(cal_, f.start_time, f.spec->new_file_freq,
                       f.spec->new_file_freq_units, &f.next_open, &aerr) ||
          !addInterval(cal_, f.start_time, dur, dur_u, &f.close_time, &aerr)) {
        if (err) *err = aerr;
        return -1;
      }
      rolled = true;
    }
    if (rolled && f.file) closePhysical(f);
    drop = rec_time > f.close_time;
  }

  if (!drop) {
    if (ensureOpen(f, fname_time, err) != 0) return -1;
    IO::File& file = *f.file;
    const double tstamp = toAxisUnits(f, rec_time);
    const int frames_before = file.numTimes();

    std::vector<double> out((size_t)fr.npts);
    if (!s.acc->value(out.data()) && !at_end) {
      // FMS reports "no data available" here; the prototype writes the raw
      // (EMPTY/missing) buffer, which is what FMS itself does at diag end.
      std::fprintf(stderr, "TIM diag: empty window written for %s\n",
                   s.spec->output_name.c_str());
    }
    int rc;
    if (fr.decomposed) {
      rc = file.writeDecomposed(s.spec->output_name, out.data(), tstamp);
    } else {
      rc = file.writePlain(s.spec->output_name, out.data(), (int)fr.npts,
                           tstamp);
    }
    if (rc != 0) {
      if (err) *err = "write " + s.spec->output_name + " failed";
      return -1;
    }
    // First stream of a new record carries the averaging metadata.
    if (f.time_ops && file.numTimes() > frames_before) {
      const double t1 = toAxisUnits(f, s.last_output);
      const double t2 = toAxisUnits(f, s.next_output);
      const double dt = t2 - t1;
      const double bnds[2] = {t1, t2};
      file.writePlain("average_T1", &t1, 1, tstamp);
      file.writePlain("average_T2", &t2, 1, tstamp);
      file.writePlain("average_DT", &dt, 1, tstamp);
      file.writePlain("time_bounds", bnds, 2, tstamp);
    }
  }

  if (at_end) return 0;  // FMS at_diag_end: no advance, no reset

  s.last_output = s.next_output;
  if (freq == 0) {
    s.next_output = end_time;  // EVERY_TIME: end_time carries the post time
    s.next_next_output = end_time;
  } else if (freq > 0) {
    std::string aerr;
    s.next_output = s.next_next_output;
    if (!addInterval(cal_, s.next_next_output, freq, f.spec->output_freq_units,
                     &s.next_next_output, &aerr)) {
      if (err) *err = aerr;
      return -1;
    }
  }
  s.acc->reset();
  return 0;
}

bool DiagManager::post(int field_id, const TimeStamp& time, const double* data,
                       const std::uint8_t* mask, const double* rmask,
                       double weight, std::string* err) {
  if (field_id < 0 || field_id >= (int)fields_.size()) return false;
  FieldRec& fr = fields_[(size_t)field_id];

  for (int si : fr.streams) {
    Stream& s = streams_[(size_t)si];
    if (fr.opts.is_static) {  // statics: snapshot kept, written at close
      s.acc->accumulate(data, mask, rmask, 1.0);
      continue;
    }
    const int freq = files_[(size_t)s.outfile].spec->output_freq;
    if (freq == 0) {  // EVERY_TIME: each post is its own record
      s.acc->accumulate(data, mask, rmask, weight);
      s.next_output = time;
      if (writeWindow(s, false, time, err) != 0) return false;
      continue;
    }
    // The FMS trigger: strictly time > next_output, checked BEFORE this
    // call's sample is accumulated — the triggering sample opens the next
    // window.
    if (freq > 0 && time > s.next_output) {
      if (writeWindow(s, false, time, err) != 0) return false;
    }
    s.acc->accumulate(data, mask, rmask, weight);
  }
  return true;
}

void DiagManager::closePhysical(OutFile& f) {
  if (!f.file) return;
  // Statics are written once per physical file, at close (FMS rule 8).
  for (int si : f.streams) {
    Stream& s = streams_[(size_t)si];
    const FieldRec& fr = fields_[(size_t)s.field];
    if (!fr.opts.is_static || s.acc->empty()) continue;
    std::vector<double> out((size_t)fr.npts);
    s.acc->value(out.data());
    if (fr.decomposed)
      f.file->writeDecomposed(s.spec->output_name, out.data(), std::nullopt);
    else
      f.file->writePlain(s.spec->output_name, out.data(), (int)fr.npts,
                         std::nullopt);
  }
  f.file.reset();  // destructor closes
}

int DiagManager::end(const TimeStamp& time) {
  std::string err;
  int rc = 0;
  for (auto& s : streams_) {
    if (fields_[(size_t)s.field].opts.is_static) continue;
    const int freq = files_[(size_t)s.outfile].spec->output_freq;
    // FMS closing flush: >= (not the send_data >); END_OF_RUN always writes.
    if (freq < 0 || (freq > 0 && time >= s.next_output)) {
      if (writeWindow(s, true, time, &err) != 0) {
        std::fprintf(stderr, "TIM diag end: %s\n", err.c_str());
        rc = -1;
      }
    }
  }
  for (auto& f : files_) {
    // Statics-only files were never opened by a record write.
    if (!f.file) {
      bool has_static = false;
      for (int si : f.streams) {
        const Stream& s = streams_[(size_t)si];
        if (fields_[(size_t)s.field].opts.is_static && !s.acc->empty())
          has_static = true;
      }
      if (has_static && ensureOpen(f, time, &err) != 0) {
        std::fprintf(stderr, "TIM diag end: %s\n", err.c_str());
        rc = -1;
        continue;
      }
    }
    closePhysical(f);
  }
  return rc;
}

// ---------------------------------------------------------------------------
// Restart-spanning averaging (Q5): every stream's window + accumulation
// persists through the same TIM::IO path as everything else.

namespace {
std::string streamStateKey(const std::string& module, const std::string& field,
                           const FieldSpec& spec) {
  return lower(module) + "|" + lower(field) + "|" + spec.file_name + "|" +
         spec.output_name;
}
}  // namespace

int DiagManager::saveState(const std::string& path) {
  // One shared domain serves every decomposed stream (enforced per file at
  // registration; across files MOM6 uses the single ocean domain).
  int dkey = -1;
  Decomp2D dom;
  for (const auto& f : files_) {
    if (f.has_domain) {
      if (dkey >= 0 && f.domain_key != dkey) {
        std::fprintf(stderr, "TIM diag saveState: multiple domains\n");
        return -1;
      }
      dkey = f.domain_key;
      dom = f.domain;
    }
  }
  auto file = IO::File::create(sys_, path, dkey, dom,
                               IO::File::Mode::Overwrite);
  if (!file) return -1;

  file->putGlobalAtt("TIM_diag_state", "1");
  file->putGlobalAtt("state_count", std::to_string(streams_.size()));

  // Geometry dims shared across streams: one x/y axis per stagger component,
  // one fixed dim per distinct length.
  auto xname = [&](Stagger st) {
    return staggeredX(st) ? std::string("sx_e") : std::string("sx_c");
  };
  auto yname = [&](Stagger st) {
    return staggeredY(st) ? std::string("sy_n") : std::string("sy_c");
  };
  std::vector<std::string> defined;
  auto ensureAxis = [&](const std::string& name, IO::File::AxisKind kind,
                        Stagger pos, int len) {
    if (std::find(defined.begin(), defined.end(), name) != defined.end())
      return;
    defined.push_back(name);
    file->defineAxis(name, kind, pos, len, "", "", "", std::nullopt);
  };

  char att[64];
  for (size_t i = 0; i < streams_.size(); ++i) {
    const Stream& s = streams_[i];
    const FieldRec& fr = fields_[(size_t)s.field];
    std::snprintf(att, sizeof att, "s%zu_key", i);
    file->putGlobalAtt(att, streamStateKey(fr.module, fr.name, *s.spec));
    std::snprintf(att, sizeof att, "s%zu_window", i);
    char win[128];
    std::snprintf(win, sizeof win, "%d %d %d %d %d %d %d", s.last_output.days,
                  s.last_output.seconds, s.next_output.days,
                  s.next_output.seconds, s.next_next_output.days,
                  s.next_next_output.seconds, fr.decomposed ? 1 : 0);
    file->putGlobalAtt(att, win);

    const std::string vn = "acc" + std::to_string(i);
    if (fr.decomposed) {
      ensureAxis(xname(fr.stagger), IO::File::AxisKind::X,
                 staggeredX(fr.stagger) ? Stagger::EastFace : Stagger::Center,
                 0);
      ensureAxis(yname(fr.stagger), IO::File::AxisKind::Y,
                 staggeredY(fr.stagger) ? Stagger::NorthFace : Stagger::Center,
                 0);
      std::vector<std::string> dims{xname(fr.stagger), yname(fr.stagger)};
      if (fr.nz > 1) {
        const std::string zn = "dim" + std::to_string(fr.nz);
        ensureAxis(zn, IO::File::AxisKind::Fixed, Stagger::Center, fr.nz);
        dims.push_back(zn);
      }
      file->defineVar(vn, dims, "", "", "", false, "");
      if (fr.opts.mask_variant) file->defineVar(vn + "_ctr", dims, "", "", "", false, "");
    } else {
      const std::string zn = "dim" + std::to_string(fr.npts);
      ensureAxis(zn, IO::File::AxisKind::Fixed, Stagger::Center, (int)fr.npts);
      file->defineVar(vn, {zn}, "", "", "", false, "");
      if (fr.opts.mask_variant) file->defineVar(vn + "_ctr", {zn}, "", "", "", false, "");
    }
    // count0d + num_elements, packed pairwise.
    ensureAxis("dim2", IO::File::AxisKind::Fixed, Stagger::Center, 2);
    file->defineVar(vn + "_cnt", {"dim2"}, "", "", "", false, "");
  }

  for (size_t i = 0; i < streams_.size(); ++i) {
    const Stream& s = streams_[i];
    const FieldRec& fr = fields_[(size_t)s.field];
    const std::string vn = "acc" + std::to_string(i);
    const Accumulator::State st = s.acc->state();
    if (fr.decomposed) {
      file->writeDecomposed(vn, st.buffer.data(), std::nullopt);
      if (fr.opts.mask_variant)
        file->writeDecomposed(vn + "_ctr", st.counter.data(), std::nullopt);
    } else {
      file->writePlain(vn, st.buffer.data(), (int)fr.npts, std::nullopt);
      if (fr.opts.mask_variant)
        file->writePlain(vn + "_ctr", st.counter.data(), (int)fr.npts,
                         std::nullopt);
    }
    const double cnt[2] = {st.count0d[0], st.num_elements[0]};
    file->writePlain(vn + "_cnt", cnt, 2, std::nullopt);
  }
  return file->close();
}

int DiagManager::restoreState(const std::string& path) {
  auto file = IO::File::openForRead(sys_, path);
  if (!file) return 1;  // absent = cold start; every window starts fresh
  auto marker = file->globalAttText("TIM_diag_state");
  if (!marker) return 1;

  int count = 0;
  if (auto c = file->globalAttText("state_count")) count = std::atoi(c->c_str());
  std::map<std::string, int> saved;
  char att[64];
  for (int i = 0; i < count; ++i) {
    std::snprintf(att, sizeof att, "s%d_key", i);
    if (auto k = file->globalAttText(att)) saved[*k] = i;
  }

  for (auto& s : streams_) {
    const FieldRec& fr = fields_[(size_t)s.field];
    auto it = saved.find(streamStateKey(fr.module, fr.name, *s.spec));
    if (it == saved.end()) continue;  // new table entry: fresh window
    const int i = it->second;

    std::snprintf(att, sizeof att, "s%d_window", i);
    auto win = file->globalAttText(att);
    if (!win) continue;
    TimeStamp lo, no, nn;
    int was_dd = 0;
    if (std::sscanf(win->c_str(), "%d %d %d %d %d %d %d", &lo.days,
                    &lo.seconds, &no.days, &no.seconds, &nn.days, &nn.seconds,
                    &was_dd) != 7)
      continue;
    if ((was_dd != 0) != fr.decomposed) continue;  // geometry changed

    const std::string vn = "acc" + std::to_string(i);
    Accumulator::State st = s.acc->state();  // correct shapes, then refill
    int rc;
    if (fr.decomposed) {
      rc = file->readDecomposed(vn, fr.domain_key, fr.domain, fr.stagger, -1,
                                fr.nz, fr.nz2, st.buffer.data());
      if (rc == 0 && fr.opts.mask_variant)
        rc = file->readDecomposed(vn + "_ctr", fr.domain_key, fr.domain,
                                  fr.stagger, -1, fr.nz, fr.nz2,
                                  st.counter.data());
    } else {
      rc = file->readPlain(vn, -1, (int)fr.npts, st.buffer.data());
      if (rc == 0 && fr.opts.mask_variant)
        rc = file->readPlain(vn + "_ctr", -1, (int)fr.npts, st.counter.data());
    }
    if (rc != 0) continue;
    double cnt[2] = {0.0, 0.0};
    if (file->readPlain(vn + "_cnt", -1, 2, cnt) != 0) continue;
    st.count0d[0] = cnt[0];
    st.num_elements[0] = cnt[1];
    if (!s.acc->restore(st)) continue;
    s.last_output = lo;
    s.next_output = no;
    s.next_next_output = nn;
  }
  return 0;
}

}  // namespace Diag
}  // namespace TIM
