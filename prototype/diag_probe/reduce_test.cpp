// Accumulator unit checks against the FMS semantics spec
// (docs/fms_diag_semantics.md). No MPI; plain asserts.
#include "../../tim/cpp/diag/tim_diag_reduce.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using TIM::Diag::Accumulator;
using TIM::Diag::Reduction;

static int failures = 0;
#define CHECK(cond, what)                                   \
  do {                                                      \
    if (!(cond)) {                                          \
      std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
      ++failures;                                           \
    }                                                       \
  } while (0)

int main() {
  const double MISS = -1e34;

  {  // weighted mean: sum(w*x)/sum(w), divide at output
    Accumulator a(3, Reduction::Mean, 1, std::nullopt, false);
    double x1[3] = {1, 2, 3}, x2[3] = {5, 6, 7}, out[3];
    a.accumulate(x1, nullptr, nullptr, 2.0);
    a.accumulate(x2, nullptr, nullptr, 1.0);
    CHECK(a.value(out), "mean has data");
    CHECK(out[0] == (2.0 * 1 + 5) / 3.0, "mean[0]");
    CHECK(out[2] == (2.0 * 3 + 7) / 3.0, "mean[2]");
  }

  {  // scalar per-CALL counter: masked cell divided by the same count
    Accumulator a(2, Reduction::Mean, 1, MISS, false);
    double x[2] = {10, 20}, out[2];
    std::uint8_t m1[2] = {1, 1}, m2[2] = {1, 0};
    a.accumulate(x, m1, nullptr, 1.0);
    a.accumulate(x, m2, nullptr, 1.0);  // pt1 masked on 2nd call
    CHECK(a.value(out), "masked mean has data");
    CHECK(out[0] == 20.0 / 2.0, "unmasked cell: sum 20 / count 2");
    CHECK(out[1] == MISS, "masked-late cell OVERWRITTEN to missing");
  }

  {  // mask ignored when no missing_value registered (FMS warning case)
    Accumulator a(2, Reduction::Mean, 1, std::nullopt, false);
    double x[2] = {4, 8}, out[2];
    std::uint8_t m[2] = {1, 0};
    a.accumulate(x, m, nullptr, 1.0);
    a.value(out);
    CHECK(out[1] == 8.0, "mask ignored without missing_value");
  }

  {  // mask_variant: per-point counters, no overwrite
    Accumulator a(2, Reduction::Mean, 1, MISS, true);
    double x[2] = {10, 20}, out[2];
    std::uint8_t m1[2] = {1, 1}, m2[2] = {1, 0};
    a.accumulate(x, m1, nullptr, 1.0);
    a.accumulate(x, m2, nullptr, 1.0);
    a.value(out);
    CHECK(out[0] == 10.0, "mask_variant pt0: 20/2");
    CHECK(out[1] == 20.0, "mask_variant pt1: 20/1 (no overwrite)");
  }

  {  // rms: accumulate (x*w)^2, divide by sum(w), sqrt
    Accumulator a(1, Reduction::RMS, 2, std::nullopt, false);
    double x1[1] = {3}, x2[1] = {4}, out[1];
    a.accumulate(x1, nullptr, nullptr, 1.0);
    a.accumulate(x2, nullptr, nullptr, 1.0);
    a.value(out);
    CHECK(std::abs(out[0] - std::sqrt((9.0 + 16.0) / 2.0)) < 1e-15, "rms");
  }

  {  // rms weight raised to the power too (FMS quirk)
    Accumulator a(1, Reduction::RMS, 2, std::nullopt, false);
    double x[1] = {3}, out[1];
    a.accumulate(x, nullptr, nullptr, 2.0);  // accumulates (3*2)^2 = 36
    a.value(out);
    CHECK(std::abs(out[0] - std::sqrt(36.0 / 2.0)) < 1e-15,
          "rms weight in power");
  }

  {  // snapshot: last write wins; count set even for snapshots
    Accumulator a(1, Reduction::None, 1, std::nullopt, false);
    double x1[1] = {1}, x2[1] = {2}, out[1];
    a.accumulate(x1, nullptr, nullptr, 1.0);
    a.accumulate(x2, nullptr, nullptr, 1.0);
    CHECK(a.value(out), "snapshot has data");
    CHECK(out[0] == 2.0, "snapshot last write");
  }

  {  // min/max: +-HUGE init; untouched -> missing at output
    Accumulator a(2, Reduction::Max, 1, MISS, false);
    double x[2] = {5, 7}, out[2];
    std::uint8_t m[2] = {1, 0};
    a.accumulate(x, m, nullptr, 1.0);
    a.value(out);
    CHECK(out[0] == 5.0, "max kept");
    CHECK(out[1] == MISS, "max untouched -> missing");
  }

  {  // rmask post-pass overwrites regardless of reduction
    Accumulator a(2, Reduction::Mean, 1, MISS, false);
    double x[2] = {10, 20}, rm[2] = {1.0, 0.2}, out[2];
    a.accumulate(x, nullptr, rm, 1.0);
    a.value(out);
    CHECK(out[0] == 10.0, "rmask keeps >=0.5");
    CHECK(out[1] == MISS, "rmask<0.5 -> missing");
  }

  {  // empty window: value() returns false, raw buffer emitted
    Accumulator a(1, Reduction::Mean, 1, MISS, false);
    double out[1] = {42};
    CHECK(!a.value(out), "empty window reports false");
    CHECK(out[0] == 0.0, "empty window emits EMPTY buffer");
  }

  {  // diurnal samples are independent
    Accumulator a(1, Reduction::Diurnal, 1, std::nullopt, false, 2);
    double x1[1] = {10}, x2[1] = {30}, out[1];
    a.accumulate(x1, nullptr, nullptr, 1.0, 0);
    a.accumulate(x2, nullptr, nullptr, 1.0, 1);
    a.value(out, 0);
    CHECK(out[0] == 10.0, "diurnal sample 0");
    a.value(out, 1);
    CHECK(out[0] == 30.0, "diurnal sample 1");
  }

  {  // Q5 persistence: state -> fresh accumulator -> restore -> identical
    Accumulator a(3, Reduction::Mean, 1, MISS, false);
    double x[3] = {1, 2, 3}, outA[3], outB[3];
    std::uint8_t m[3] = {1, 1, 0};
    a.accumulate(x, m, nullptr, 2.5);
    auto st = a.state();
    Accumulator b(3, Reduction::Mean, 1, MISS, false);
    CHECK(b.restore(st), "restore accepts matching shape");
    a.accumulate(x, m, nullptr, 1.5);
    b.accumulate(x, m, nullptr, 1.5);
    a.value(outA);
    b.value(outB);
    for (int i = 0; i < 3; ++i)
      CHECK(outA[i] == outB[i], "restored stream bit-identical");
    Accumulator c(4, Reduction::Mean, 1, MISS, false);
    CHECK(!c.restore(st), "restore rejects shape mismatch");
  }

  {  // reset restores init values (incl. max's -HUGE)
    Accumulator a(1, Reduction::Max, 1, MISS, false);
    double x[1] = {5}, out[1];
    a.accumulate(x, nullptr, nullptr, 1.0);
    a.reset();
    a.value(out);
    CHECK(out[0] == MISS, "reset max back to untouched");
    CHECK(a.empty(), "reset -> empty");
  }

  if (failures == 0) std::printf("ALL ACCUMULATOR CHECKS PASS\n");
  return failures == 0 ? 0 : 1;
}
