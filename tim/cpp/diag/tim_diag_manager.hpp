#pragma once
// TIM::Diag::DiagManager — the deep module of TIM diagnostics (designed pass).
//
// One class stands in for FMS's diag_manager/diag_util/diag_output triple:
// diag_table-driven registration and fan-out (one field -> N output streams),
// the window scheduler (strict `time > next_output` trigger, next/next_next
// advance through calendar months), file lifecycle (lazy open, new_file_freq
// rollover with FMS %-token filename stamping, statics at close), and the
// FMS-format metadata (axis/positive atts, time calendar+bounds, average_T1/
// T2/DT + time_bounds, _FillValue+missing_value with the CMOR 1e20 default,
// time_avg_info, cell_methods time suffix). Reduction math lives in
// Accumulator; all netCDF/PIO access goes through TIM::IO::File — this
// module never touches pio.h or AMReX.
//
// Semantics contract: docs/fms_diag_semantics.md (the 12 reproduce-exactly
// rules). Deliberate divergences from FMS, all documented in the findings:
//   * output windows are anchored on the diag_table base date and walked
//     forward past init_time — NOT anchored at init_time as FMS does — so a
//     mid-window restart resumes the same window (identical to FMS whenever
//     the run starts on a window boundary, i.e. always in practice);
//   * restart-spanning averaging: saveState()/restoreState() persist every
//     in-progress Accumulator plus its window through TIM::IO (FMS silently
//     corrupts partial windows across restarts);
//   * no global state: calendar, config, and the IoSystem are explicit;
//     ensemble members each own a DiagManager;
//   * errors are return codes/messages, never FATAL.
// Not implemented in the prototype (registration fails cleanly): regional
// output, diurnalNN files, grid coarsening.

#include "../core/tim_domain.hpp"
#include "../core/tim_time.hpp"
#include "../io/tim_file.hpp"
#include "tim_diag_axis.hpp"
#include "tim_diag_config.hpp"
#include "tim_diag_reduce.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace TIM {
namespace IO { class IoSystem; }

namespace Diag {

constexpr int kFieldNotFound = -1;         // FMS DIAG_FIELD_NOT_FOUND
constexpr double kCmorMissing = 1.0e20;    // FMS CMOR_MISSING_VALUE default

struct FieldOptions {
  std::string long_name, units, standard_name, interp_method;
  std::optional<double> missing_value;     // default: CMOR 1e20 in the file
  std::optional<std::pair<double, double>> valid_range;
  bool mask_variant = false;
  bool is_static = false;
  int area_field = kFieldNotFound;    // diag id -> "cell_measures: area: <name>"
  int volume_field = kFieldNotFound;  //         -> "... volume: <name>"
};

class DiagManager {
 public:
  struct Options {
    std::string title;          // global 'title' attribute (diag_table line 1)
    bool prepend_date = false;  // FMS: only when diag_manager_init got time_init
  };

  // config/calendar/init_time are the run's fixed context; sys is borrowed
  // and must outlive the manager. Check ok() after construction.
  // (Two ctors, not a defaulted Options arg: an in-class default argument
  // may not use the class's own default member initializers.)
  DiagManager(const DiagConfig& config, Calendar cal, const TimeStamp& init_time,
              IO::IoSystem& sys)
      : DiagManager(config, cal, init_time, sys, Options()) {}
  DiagManager(const DiagConfig& config, Calendar cal, const TimeStamp& init_time,
              IO::IoSystem& sys, const Options& opts);
  ~DiagManager();
  DiagManager(const DiagManager&) = delete;
  DiagManager& operator=(const DiagManager&) = delete;

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

  // ---- axes (MOM_diag_axis_init) ----
  int defineAxis(const AxisSpec& a) { return axes_.define(a); }
  std::string axisName(int id) const {
    return axes_.valid(id) ? axes_.at(id).name : std::string();
  }

  // ---- registration ----
  // Returns kFieldNotFound when the diag_table requests no output for
  // (module, field) — a non-event, exactly as FMS. Matching is
  // case-insensitive. Re-registration returns the existing id.
  // axes: registry ids in Fortran order (x, y, then fixed dims); empty or
  // {kNullAxis} = scalar. Statics pass is_static in opts.
  int registerField(const std::string& module, const std::string& field,
                    const std::vector<int>& axes, const FieldOptions& opts);
  int fieldId(const std::string& module, const std::string& field) const;
  // Attribute puts before the first write reach the files; repeated text
  // attributes append with a space separator (FMS cell_methods convention).
  void addAttribute(int field_id, const std::string& name,
                    const std::string& text);
  void addAttribute(int field_id, const std::string& name, const int* v, int n);
  void addAttribute(int field_id, const std::string& name, const double* v,
                    int n);

  // ---- data path ----
  // data: the field's full local window at its registered axes — for
  // decomposed fields Decomp2D::window(stagger) layout (x fastest, then y,
  // then the fixed dims), replicated values for scalar/replicated fields.
  // mask: true = use the point; rmask: < 0.5 = masked (FMS post-pass).
  // The is_in/ie_in halo bookkeeping is the Fortran bridge's job; by the time
  // data lands here it is exactly the window. Collective when any stream of
  // the field triggers a window close. Returns false with *err on error.
  bool post(int field_id, const TimeStamp& time, const double* data,
            const std::uint8_t* mask = nullptr, const double* rmask = nullptr,
            double weight = 1.0, std::string* err = nullptr);
  void sendComplete() {}  // classic path: nothing deferred
  void setTimeEnd(const TimeStamp& t) { time_end_ = t; }
  // Final flush (FMS diag_manager_end): windows with time >= next_output
  // (note >=) are written without advancing; END_OF_RUN streams close at
  // `time`; statics are written; all files close. The trailing partial
  // window is NOT written (FMS behavior) — saveState() carries it instead.
  int end(const TimeStamp& time);

  // ---- restart-spanning averaging (Q5; capability FMS lacks) ----
  // Persist every stream's in-progress accumulation + window to `path`
  // (collective, through TIM::IO with the same decompositions). Restore
  // matches streams by (module, field, file, output_name); a missing match
  // starts a fresh window; a missing file is a cold start (returns 1).
  int saveState(const std::string& path);
  int restoreState(const std::string& path);

 private:
  struct Attr {
    enum class Kind { Text, Ints, Reals } kind;
    std::string name, text;
    std::vector<int> ints;
    std::vector<double> reals;
  };

  struct FieldRec {
    std::string module, name;   // as registered
    std::vector<int> axes;      // registry ids (kNullAxis stripped)
    FieldOptions opts;
    std::vector<Attr> attrs;
    // derived geometry
    bool decomposed = false;
    int domain_key = -1;
    Decomp2D domain;
    Stagger stagger = Stagger::Center;
    int nz = 1, nz2 = 1;        // product of fixed-dim lengths; innermost
    long long npts = 1;         // local window points (x*y*fixed)
    std::vector<int> streams;
  };

  struct Stream {
    int field = -1;
    int outfile = -1;
    const FieldSpec* spec = nullptr;
    bool time_ops = false;      // averaged reduction (Mean/RMS/Pow)
    std::unique_ptr<Accumulator> acc;
    TimeStamp last_output, next_output, next_next_output;
  };

  struct OutFile {
    const FileSpec* spec = nullptr;
    std::vector<int> streams;
    std::optional<IO::File> file;   // open lazily at first record
    TimeStamp start_time, close_time, next_open;  // rollover trio
    bool rollover = false;          // new_file_freq present
    bool time_ops = false;          // any averaged stream -> average_T* vars
    bool has_domain = false;
    int domain_key = -1;
    Decomp2D domain;
    std::string time_units_str;     // "<units> since <base date>"
    double unit_seconds = 86400.0;  // seconds per time-axis unit
  };

  int streamWindowsInit(Stream& s);
  int ensureOpen(OutFile& f, const TimeStamp& fname_time, std::string* err);
  int defineFileContents(OutFile& f);
  // Window close for one stream: rollover/drop bookkeeping, the record write,
  // average_T*/time_bounds once per new frame, then advance+reset (skipped
  // when at_end). Collective.
  int writeWindow(Stream& s, bool at_end, const TimeStamp& end_time,
                  std::string* err);
  void closePhysical(OutFile& f);   // statics, then close
  double toAxisUnits(const OutFile& f, const TimeStamp& t) const;
  std::string filePath(const OutFile& f, const TimeStamp& fname_time) const;

  const DiagConfig& cfg_;
  Calendar cal_;
  TimeStamp base_time_, init_time_, time_end_;
  IO::IoSystem& sys_;
  Options opts_;
  bool ok_ = false;
  std::string error_;

  AxisRegistry axes_;
  std::vector<FieldRec> fields_;
  std::map<std::string, int> field_ids_;  // lower(module)|lower(field) -> idx
  std::vector<Stream> streams_;
  std::vector<OutFile> files_;
};

// Exposed for host-only unit tests: the literal port of FMS get_time_string —
// %-token suffix (always '-'-joined, leading '.') stamped from t, and the
// base name truncated at the first %<digit>.
std::string fmsTimeSuffix(const std::string& name, Calendar cal,
                          const TimeStamp& t);
std::string fmsBaseName(const std::string& name, bool* has_percent = nullptr);

}  // namespace Diag
}  // namespace TIM
