// Gate 1c capture tool: dumps the DOF lists the CURRENT tim_decomp_cache.cpp
// formula produces for a representative set of windows/staggers/depths, so the
// refactored DofMapCache generator can be proven bit-identical.
//
// Host-only (no PIO): it embeds a verbatim copy of DecompCache::build's dof
// loop and drives it through the unchanged TIM::Decomp2D geometry. Run once
// before the refactor to write the golden file; dof_identity.cpp reads it back
// after the refactor and compares.
//
//   g++ -std=c++20 -O2 -o dof_capture dof_capture.cpp && ./dof_capture > dof_golden.txt

#include "../../tim/cpp/core/tim_domain.hpp"

#include <cstdio>
#include <vector>

using TIM::Decomp2D;
using TIM::Stagger;
using TIM::Window;

// --- verbatim copy of the dof loop + gdims logic from tim_decomp_cache.cpp ---
static std::vector<long long> dofList(const Decomp2D& d, Stagger stagger,
                                      const Window& w, int nz) {
  const int gnx = d.globalNx(stagger), gny = d.globalNy(stagger);
  std::vector<long long> dof;
  if (!w.empty()) {
    dof.reserve((size_t)w.npts() * nz);
    for (int k = 0; k < nz; ++k)
      for (int j = w.js; j <= w.je; ++j)
        for (int i = w.is; i <= w.ie; ++i)
          dof.push_back((long long)k * gnx * gny + (long long)(j - 1) * gnx + i);
  }
  return dof;
}

static void dumpCase(const char* label, const Decomp2D& d, Stagger s, int nz) {
  // Read components (up to 4) and the write partition — every window family
  // the decomp cache builds.
  Window comps[4];
  const int ncomp = d.readComponents(s, comps);
  for (int c = 0; c < ncomp; ++c) {
    auto dof = dofList(d, s, comps[c], nz);
    std::printf("%s stag=%d READ comp=%d nz=%d n=%zu:", label, (int)s, c, nz,
                dof.size());
    for (long long v : dof) std::printf(" %lld", v);
    std::printf("\n");
  }
  auto pw = dofList(d, s, d.writePartition(s), nz);
  std::printf("%s stag=%d WRITE nz=%d n=%zu:", label, (int)s, nz, pw.size());
  for (long long v : pw) std::printf(" %lld", v);
  std::printf("\n");
}

int main() {
  // Representative single-rank windows on a 100x80 global grid (symmetric),
  // mirroring the cesm_t232 masked-tripolar edge cases: an interior rank, an
  // east-edge rank (ie==nig), a north-edge rank (je==njg), the NE corner rank,
  // and an all-land (empty) rank.
  const int NIG = 100, NJG = 80;
  struct RankCase { const char* name; int is, ie, js, je; };
  const RankCase ranks[] = {
      {"interior", 21, 40, 21, 40},
      {"east-edge", 81, 100, 21, 40},
      {"north-edge", 21, 40, 61, 80},
      {"ne-corner", 81, 100, 61, 80},
      {"empty", 1, 0, 1, 0},
  };
  const Stagger staggers[] = {Stagger::Center, Stagger::EastFace,
                              Stagger::NorthFace, Stagger::Corner};
  const int depths[] = {1, 5};

  for (const auto& r : ranks) {
    Decomp2D d(NIG, NJG, r.is, r.ie, r.js, r.je, /*symmetric=*/true);
    for (Stagger s : staggers)
      for (int nz : depths) dumpCase(r.name, d, s, nz);
  }
  // Also a non-symmetric grid (staggered axes do NOT grow).
  for (const auto& r : ranks) {
    Decomp2D d(NIG, NJG, r.is, r.ie, r.js, r.je, /*symmetric=*/false);
    for (Stagger s : staggers) dumpCase("nosym", d, s, 1);
  }
  return 0;
}
