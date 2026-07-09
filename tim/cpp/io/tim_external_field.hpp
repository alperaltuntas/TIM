#pragma once
// TIM::IO::ExternalField — time-interpolating reader for external forcing
// fields (the FMS time_interp_external2 replacement; designed pass).
//
// Reproduces the FMS semantics EXACTLY for on-grid fields (the only mode the
// MOM6 seam uses — init_extern_field hard-codes ongrid):
//   * time axis converted via the get_cal_time rules (floor/truncate split of
//     "days|hours|minutes|seconds since <date>"; file calendar must match the
//     model calendar);
//   * a "modulo" time-axis attribute selects year-periodic climatology
//     bracketing (time_interp_list with modtime=YEAR: record years rewritten
//     to year 1, model date mapped into the climatology year with the FMS
//     Feb-29 correction, wraparound weights across the ends);
//   * weights via exact integer-second time arithmetic divided in double
//     (time_divide), blend = r1*(1-w2) + r2*w2 where BOTH records are valid,
//     else the variable's missing value (_FillValue > missing_value >
//     "missing" > netCDF double fill);
//   * validity per fms2_io get_valid: scale/offset-adjusted valid_range /
//     valid_min / valid_max, the fill-derived open range (two ULPs inside the
//     fill), and exact mismatch against fill/missing.
// Records are read through the same decomposed TIM read path as restarts
// (File::readDecomposed) with a two-record round-robin cache, or replicated
// reads for fields without a decomposition. No FMS anywhere.

#include "../core/tim_domain.hpp"
#include "../core/tim_time.hpp"
#include "tim_file.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace TIM {
namespace IO {

class IoSystem;

class ExternalField {
 public:
  // Resolves `field` case-insensitively in `path` and indexes the time axis.
  // domain_key < 0 = replicated field (no decomposition). Check ok().
  ExternalField(IoSystem& sys, const std::string& path,
                const std::string& field, int domain_key,
                const Decomp2D& domain, Calendar cal);

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  const std::string& varName() const { return varname_; }
  double missingValue() const { return missing_; }
  // Fortran-order global sizes (x, y, z, t).
  void sizes(int siz[4]) const {
    for (int k = 0; k < 4; ++k) siz[k] = siz_[k];
  }
  // Local buffer length interp() fills: window npts * nz (or global for
  // replicated fields).
  long long npts() const { return npts_; }
  // Local window shape of that buffer (x-fastest layout).
  void window(int* ni, int* nj, int* nz) const {
    if (decomposed_) {
      const Window w = domain_.window(Stagger::Center);
      *ni = w.ni(); *nj = w.nj();
    } else {
      *ni = siz_[0]; *nj = siz_[1];
    }
    *nz = nz_;
  }

  // Interpolate to model time t. out: npts() values, window layout (x
  // fastest). mask (optional): 1 where both bracketing records are valid.
  // Nonzero on error (message via error()).
  int interp(const TimeStamp& t, double* out, unsigned char* mask);

 private:
  int loadRecord(int rec, int* slot, int avoid = -1);
  int bracket(const TimeStamp& t, double* w2, int* rec1, int* rec2);
  bool valid(double x) const;

  IoSystem& sys_;
  std::string path_, varname_, timename_;
  int domain_key_ = -1;
  Decomp2D domain_;
  Calendar cal_ = Calendar::NoCalendar;
  bool ok_ = false;
  std::string error_;

  int siz_[4] = {1, 1, 1, 1};
  int nz_ = 1;
  long long npts_ = 1;
  bool decomposed_ = false;

  double missing_ = 0.0;
  // fms2_io Valid_t
  bool has_min_ = false, has_max_ = false, has_fill_ = false,
       has_missing_ = false;
  double min_val_ = 0.0, max_val_ = 0.0, fill_val_ = 0.0, missing_val_ = 0.0;

  bool modulo_ = false;
  std::vector<TimeStamp> times_;  // record times (year->1 when modulo)

  std::optional<File> file_;      // held open for decomposed record reads
  // Two-slot round-robin record cache (FMS num_io_buffers=2).
  std::vector<double> buf_[2];
  int buf_rec_[2] = {-1, -1};
  int next_slot_ = 0;
};

}  // namespace IO
}  // namespace TIM
