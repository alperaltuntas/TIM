#include "tim_diag_reduce.hpp"

#include <cmath>
#include <limits>

namespace TIM {
namespace Diag {

namespace {
constexpr double kHuge = std::numeric_limits<double>::max();
}

Accumulator::Accumulator(std::int64_t npts, Reduction reduction,
                         int pow_exponent, std::optional<double> missing_value,
                         bool mask_variant, int nsamples)
    : npts_(npts),
      reduction_(reduction),
      pow_(reduction == Reduction::RMS ? 2 : pow_exponent),
      missing_(missing_value),
      mask_variant_(mask_variant && missing_value.has_value()),
      nsamples_(nsamples < 1 ? 1 : nsamples) {
  buffer_.assign((std::size_t)npts_ * nsamples_, initValue());
  if (mask_variant_) counter_.assign((std::size_t)npts_ * nsamples_, 0.0);
  count0d_.assign(nsamples_, 0.0);
  num_elements_.assign(nsamples_, 0.0);
}

double Accumulator::initValue() const {
  // FMS: EMPTY=0.0 for mean/sum/point; buffers start at -HUGE for max and
  // +HUGE for min so the first comparison always wins.
  if (reduction_ == Reduction::Max) return -kHuge;
  if (reduction_ == Reduction::Min) return kHuge;
  return 0.0;
}

void Accumulator::accumulate(const double* data, const std::uint8_t* lmask,
                             const double* rmask, double weight, int sample) {
  double* b = buf(sample);
  const double miss = missing_.value_or(0.0);
  const bool have_miss = missing_.has_value();
  // FMS: a logical mask without a registered missing_value is IGNORED
  // (one-time warning) — the accumulation runs as unmasked.
  const std::uint8_t* m = have_miss ? lmask : nullptr;

  bool any_unmasked = false;

  switch (reduction_) {
    case Reduction::Mean:
    case Reduction::RMS:
    case Reduction::Pow:
    case Reduction::Diurnal: {  // diurnal is mean with a sample index
      if (mask_variant_) {
        double* c = counter_.data() + (std::size_t)sample * npts_;
        for (std::int64_t i = 0; i < npts_; ++i) {
          if (!m || m[i]) {
            const double t = data[i] * weight;
            b[i] += (pow_ != 1) ? std::pow(t, pow_) : t;
            c[i] += weight;
            any_unmasked = true;
          }
          // mask_variant leaves masked cells untouched (counter stays 0)
        }
      } else if (m) {
        for (std::int64_t i = 0; i < npts_; ++i) {
          if (m[i]) {
            const double t = data[i] * weight;
            b[i] += (pow_ != 1) ? std::pow(t, pow_) : t;
            any_unmasked = true;
          } else {
            b[i] = miss;  // FMS overwrites a masked point's accumulation
          }
        }
        if (any_unmasked) count0d_[sample] += weight;
      } else if (have_miss) {
        // no mask, missing present: points equal to missing skip and hold
        for (std::int64_t i = 0; i < npts_; ++i) {
          if (data[i] != miss) {
            const double t = data[i] * weight;
            b[i] += (pow_ != 1) ? std::pow(t, pow_) : t;
            any_unmasked = true;
          } else {
            b[i] = miss;
          }
        }
        if (any_unmasked) count0d_[sample] += weight;
      } else {
        for (std::int64_t i = 0; i < npts_; ++i) {
          const double t = data[i] * weight;
          b[i] += (pow_ != 1) ? std::pow(t, pow_) : t;
        }
        count0d_[sample] += weight;
      }
      break;
    }

    case Reduction::None: {  // snapshot: last write wins
      for (std::int64_t i = 0; i < npts_; ++i) {
        b[i] = (m && !m[i] && have_miss) ? miss : data[i];
      }
      count0d_[sample] = 1.0;
      break;
    }

    case Reduction::Max: {
      for (std::int64_t i = 0; i < npts_; ++i)
        if ((!m || m[i]) && data[i] > b[i]) b[i] = data[i];
      count0d_[sample] = 1.0;
      break;
    }
    case Reduction::Min: {
      for (std::int64_t i = 0; i < npts_; ++i)
        if ((!m || m[i]) && data[i] < b[i]) b[i] = data[i];
      count0d_[sample] = 1.0;
      break;
    }
  }
  num_elements_[sample] += (double)npts_;

  // FMS rmask post-pass: independent of the reduction, cells with
  // rmask < 0.5 are overwritten with missing_value (when registered).
  if (rmask && have_miss) {
    for (std::int64_t i = 0; i < npts_; ++i)
      if (rmask[i] < 0.5) b[i] = miss;
  }
}

bool Accumulator::value(double* out, int sample) const {
  const double* b = buf(sample);
  const double miss = missing_.value_or(0.0);
  const bool have_miss = missing_.has_value();

  const bool averaged = reduction_ == Reduction::Mean ||
                        reduction_ == Reduction::RMS ||
                        reduction_ == Reduction::Pow ||
                        reduction_ == Reduction::Diurnal;

  if (averaged && mask_variant_) {
    const double* c = counter_.data() + (std::size_t)sample * npts_;
    for (std::int64_t i = 0; i < npts_; ++i) {
      if (c[i] > 0.0) {
        double v = b[i] / c[i];
        if (reduction_ == Reduction::RMS) v = std::sqrt(v);
        out[i] = v;
      } else {
        out[i] = miss;
      }
    }
    return true;
  }

  if (averaged) {
    const double num = count0d_[sample];
    if (num <= 0.0) {  // FMS "write EMPTY buffer": raw contents go out
      for (std::int64_t i = 0; i < npts_; ++i) out[i] = b[i];
      return false;
    }
    for (std::int64_t i = 0; i < npts_; ++i) {
      if (have_miss && b[i] == miss) {
        out[i] = miss;
      } else {
        double v = b[i] / num;
        if (reduction_ == Reduction::RMS) v = std::sqrt(v);
        out[i] = v;
      }
    }
    return true;
  }

  if (reduction_ == Reduction::Max || reduction_ == Reduction::Min) {
    const double untouched = initValue();
    for (std::int64_t i = 0; i < npts_; ++i)
      out[i] = (have_miss && b[i] == untouched) ? miss : b[i];
    return count0d_[sample] > 0.0;
  }

  // snapshot
  for (std::int64_t i = 0; i < npts_; ++i) out[i] = b[i];
  return count0d_[sample] > 0.0;
}

void Accumulator::reset() {
  const double init = initValue();
  buffer_.assign(buffer_.size(), init);
  if (mask_variant_) counter_.assign(counter_.size(), 0.0);
  count0d_.assign(nsamples_, 0.0);
  num_elements_.assign(nsamples_, 0.0);
}

bool Accumulator::empty(int sample) const {
  return count0d_[(std::size_t)sample] == 0.0 &&
         num_elements_[(std::size_t)sample] == 0.0;
}

Accumulator::State Accumulator::state() const {
  return State{buffer_, counter_, count0d_, num_elements_};
}

bool Accumulator::restore(const State& s) {
  if (s.buffer.size() != buffer_.size() || s.count0d.size() != count0d_.size() ||
      s.counter.size() != counter_.size() ||
      s.num_elements.size() != num_elements_.size())
    return false;
  buffer_ = s.buffer;
  counter_ = s.counter;
  count0d_ = s.count0d;
  num_elements_ = s.num_elements;
  return true;
}

}  // namespace Diag
}  // namespace TIM
