// Gate 1c verify tool: emits the DOF lists the REFACTORED DofMapCache produces,
// in the exact format dof_capture.cpp used for the golden. Diffing this against
// dof_golden.txt (captured before the refactor from the retired DecompCache
// formula) proves the domain factory is bit-identical — same DOFs, same
// rearranger schedule, same bytes.
//
//   CC --std=c++20 -o dof_identity dof_identity.cpp \
//      ../../tim/cpp/io/tim_dofmap.cpp ../../tim/cpp/io/tim_backend.cpp \
//      -I$PIO/include -L$PIO/lib -lpioc $(nc-config --libs)
//   ./dof_identity > dof_after.txt && diff dof_golden.txt dof_after.txt
//
// It only calls the pure static DofMapCache::domainDofs — no MPI, no PIO — so it
// runs as a plain single process (no mpiexec).

#include "../../tim/cpp/core/tim_domain.hpp"
#include "../../tim/cpp/io/tim_dofmap.hpp"

#include <cstdio>
#include <vector>

using TIM::Decomp2D;
using TIM::Stagger;
using TIM::Window;
using TIM::IO::DofMapCache;

static void dumpCase(const char* label, const Decomp2D& d, Stagger s, int nz) {
  Window comps[4];
  const int ncomp = d.readComponents(s, comps);
  for (int c = 0; c < ncomp; ++c) {
    auto dof = DofMapCache::domainDofs(d, s, comps[c], nz);
    std::printf("%s stag=%d READ comp=%d nz=%d n=%zu:", label, (int)s, c, nz,
                dof.size());
    for (long long v : dof) std::printf(" %lld", v);
    std::printf("\n");
  }
  auto pw = DofMapCache::domainDofs(d, s, d.writePartition(s), nz);
  std::printf("%s stag=%d WRITE nz=%d n=%zu:", label, (int)s, nz, pw.size());
  for (long long v : pw) std::printf(" %lld", v);
  std::printf("\n");
}

int main() {
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
  for (const auto& r : ranks) {
    Decomp2D d(NIG, NJG, r.is, r.ie, r.js, r.je, /*symmetric=*/false);
    for (Stagger s : staggers) dumpCase("nosym", d, s, 1);
  }
  return 0;
}
