#pragma once
// TIM::IO::DecompCache — owns every backend decomposition TIM creates
// (prototype design pass). Hides the three decomposition families that fell
// out of the prototype findings:
//   ReadComponent  — up to 4 disjoint pieces per staggered window (overlap
//                    is illegal in PIO, so reads are stitched from pieces)
//   WritePartition — a true partition (each global point written once)
// Callers never see dof maps or backend ids' provenance.

#include "../core/tim_domain.hpp"
#include "tim_backend.hpp"

#include <map>

namespace TIM {
namespace IO {

class DecompCache {
 public:
  enum class Family : int { ReadComponent = 0, WritePartition = 1 };

  explicit DecompCache(SysId sys) : sys_(sys) {}
  ~DecompCache();
  DecompCache(const DecompCache&) = delete;
  DecompCache& operator=(const DecompCache&) = delete;

  // The decomposition for one read component / the write partition of a
  // variable at `stagger` with trailing extents nz (total) and nz2 (4th dim).
  // `domainKey` identifies the Decomp2D (registry handle); the Decomp2D
  // itself provides the geometry.
  DecompId get(int domainKey, const Decomp2D& d, Stagger stagger, int nz,
               int nz2, Family fam, int comp = 0);

 private:
  DecompId build(const Decomp2D& d, Stagger stagger, const Window& w, int nz,
                 int nz2);

  SysId sys_;
  std::map<long long, DecompId> cache_;
};

}  // namespace IO
}  // namespace TIM
