#pragma once
// TIM::Diag::Accumulator — one output stream's time reduction (designed pass).
//
// Reproduces the classic FMS diag_manager accumulation BIT-EXACTLY per
// docs/fms_diag_semantics.md. The deliberately-preserved FMS quirks:
//   * the normalization counter is a SCALAR per (stream, diurnal sample),
//     incremented once per accumulate() call when any point is unmasked —
//     not per point (per-point counting exists only in mask_variant mode);
//   * a masked point OVERWRITES its accumulated value with missing_value;
//   * for RMS/Pow the accumulated term is (x*w)^p — the weight is raised
//     to the power too — divided by sum(w), sqrt applied for RMS at output;
//   * a logical mask affects both accumulation and the call counter, while
//     rmask (< 0.5 = masked) is applied as a separate post-pass that only
//     overwrites buffer cells; both behaviors are kept distinct;
//   * Min/Max buffers start at +/-HUGE and untouched cells become
//     missing_value at output; Sum ignores the weight.
// Division happens at output (value()), never during accumulation.
//
// Persistence is first-class (restart-spanning averaging, plan Q5): state()
// exposes everything needed to reconstruct an in-progress window, restore()
// reloads it; the DiagManager serializes these through TIM::IO::File.
//
// Storage is host double (matches FMS default-REAL accumulation under the
// -fdefault-real-8 builds). The interface is pointer-based so a
// device-resident implementation can replace storage and loops without
// touching callers. No MPI, no I/O, no AMReX: pure math, unit-testable.

#include "tim_diag_config.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace TIM {
namespace Diag {

class Accumulator {
 public:
  // npts: points in the stream's window buffer (nx*ny*nz of the output
  // window). nsamples: diurnal samples (1 for everything but diurnalNN).
  Accumulator(std::int64_t npts, Reduction reduction, int pow_exponent,
              std::optional<double> missing_value, bool mask_variant,
              int nsamples = 1);

  // One send_data deposit. data: npts values (the caller's window layout).
  // lmask: optional logical mask (true = use the point); affects both the
  //   accumulation and the per-call counter, exactly as FMS's `mask`.
  // rmask: optional real mask (< 0.5 = masked); applied as the FMS post-pass
  //   (buffer overwrite only) after the accumulation.
  // weight: FMS weight (defaulted to 1.0 by the caller when absent).
  // sample: 0-based diurnal sample index.
  void accumulate(const double* data, const std::uint8_t* lmask,
                  const double* rmask, double weight, int sample = 0);

  // Emits the finalized window into out (npts values) applying the
  // divide/sqrt/missing rules; returns false when the window holds no data
  // (count == 0 for that sample) — FMS's "write EMPTY buffer" condition,
  // in which case out receives the raw buffer as FMS would write it.
  bool value(double* out, int sample = 0) const;

  void reset();                       // window rollover (writing_field reset)
  bool empty(int sample = 0) const;   // nothing accumulated this window?

  // ---- persistence (Q5): everything an in-progress window is ----
  struct State {
    std::vector<double> buffer;       // npts * nsamples
    std::vector<double> counter;      // per-point (mask_variant only) or empty
    std::vector<double> count0d;      // nsamples
    std::vector<double> num_elements; // nsamples (kept for fidelity)
  };
  State state() const;
  // Restores a state captured by state(); geometry must match construction.
  // Returns false on shape mismatch.
  bool restore(const State& s);

  std::int64_t npts() const { return npts_; }
  int nsamples() const { return nsamples_; }
  Reduction reduction() const { return reduction_; }

 private:
  double* buf(int sample) { return buffer_.data() + (std::size_t)sample * npts_; }
  const double* buf(int sample) const {
    return buffer_.data() + (std::size_t)sample * npts_;
  }
  double initValue() const;

  std::int64_t npts_;
  Reduction reduction_;
  int pow_;
  std::optional<double> missing_;
  bool mask_variant_;
  int nsamples_;

  std::vector<double> buffer_;        // npts * nsamples, weighted sums
  std::vector<double> counter_;       // npts * nsamples when mask_variant
  std::vector<double> count0d_;       // per-sample scalar weight sums
  std::vector<double> num_elements_;  // per-sample element counts (fidelity)
};

}  // namespace Diag
}  // namespace TIM
