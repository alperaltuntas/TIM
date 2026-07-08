#pragma once
// TIM::Diag axis registry (designed pass).
//
// A value store for diagnostic axes, mirroring what MOM6 hands FMS's
// diag_axis_init: global coordinate values plus placement metadata. Axis id 0
// is reserved as the null axis (scalar fields); real ids start at 1. The
// registry is dumb on purpose — all interpretation (file dims, staggering,
// decomposed vs replicated) happens in the DiagManager.

#include "../core/tim_domain.hpp"

#include <string>
#include <vector>

namespace TIM {
namespace Diag {

constexpr int kNullAxis = 0;

struct AxisSpec {
  std::string name;
  std::vector<double> values;   // GLOBAL coordinate values
  std::string units;            // "none" or empty -> no units attribute
  std::string cartesian;        // "X", "Y", "Z", "T", or "N"
  std::string long_name;
  int direction = 0;            // 1 up, -1 down, 0 non-vertical
  int edges = kNullAxis;        // axis id of the edges axis, if any
  std::string set_name;
  // Horizontal decomposed axes only:
  int domain_key = -1;          // <0 = replicated axis (Z, N, scalar sets)
  Decomp2D domain;
  bool staggered = false;       // FMS EAST/NORTH position (in own direction)
};

class AxisRegistry {
 public:
  int define(const AxisSpec& a) {
    axes_.push_back(a);
    return (int)axes_.size();  // ids are 1-based; 0 = null axis
  }
  bool valid(int id) const { return id >= 1 && id <= (int)axes_.size(); }
  const AxisSpec& at(int id) const { return axes_[(size_t)id - 1]; }

 private:
  std::vector<AxisSpec> axes_;
};

}  // namespace Diag
}  // namespace TIM
