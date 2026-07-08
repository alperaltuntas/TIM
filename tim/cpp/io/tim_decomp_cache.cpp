#include "tim_decomp_cache.hpp"

#include <vector>

namespace TIM {
namespace IO {

DecompCache::~DecompCache() {
  // Backend decompositions die with the iosystem finalize; freeing each here
  // would require the iosystem to still be alive, which the IoSystem
  // destructor guarantees by destroying the cache first.
  for (auto& kv : cache_) Backend::freeDecomp(sys_, kv.second);
}

DecompId DecompCache::get(int domainKey, const Decomp2D& d, Stagger stagger,
                          int nz, int nz2, Family fam, int comp, bool single) {
  const long long key = ((long long)domainKey << 48) |
                        ((long long)(int)stagger << 44) |
                        ((long long)(int)fam << 40) | ((long long)comp << 36) |
                        ((long long)(single ? 1 : 0) << 35) |
                        ((long long)nz2 << 20) | nz;
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second;

  Window w;
  if (fam == Family::WritePartition) {
    w = d.writePartition(stagger);
  } else {
    Window comps[4];
    const int n = d.readComponents(stagger, comps);
    w = (comp < n) ? comps[comp] : Window{};  // empty window if absent
  }
  DecompId id = build(d, stagger, w, nz, nz2, single);
  cache_[key] = id;
  return id;
}

DecompId DecompCache::build(const Decomp2D& d, Stagger stagger, const Window& w,
                            int nz, int nz2, bool single) {
  const int gnx = d.globalNx(stagger), gny = d.globalNy(stagger);

  std::vector<long long> dof;
  if (!w.empty()) {
    dof.reserve((size_t)w.npts() * nz);
    for (int k = 0; k < nz; ++k)
      for (int j = w.js; j <= w.je; ++j)
        for (int i = w.is; i <= w.ie; ++i)
          dof.push_back((long long)k * gnx * gny + (long long)(j - 1) * gnx + i);
  }

  // The backend needs the decomp's rank to match the variable's non-record
  // rank; a 4-d variable (x,y,z1,z2 in Fortran order) shares the flat index
  // of the flattened nz = nz1*nz2 3-d case.
  const int nz1 = (nz2 > 1) ? nz / nz2 : nz;
  int gdims[4];
  int ndims;
  if (nz2 > 1) {
    ndims = 4; gdims[0] = nz2; gdims[1] = nz1; gdims[2] = gny; gdims[3] = gnx;
  } else if (nz > 1) {
    ndims = 3; gdims[0] = nz; gdims[1] = gny; gdims[2] = gnx;
  } else {
    ndims = 2; gdims[0] = gny; gdims[1] = gnx;
  }
  return Backend::initDecomp(sys_, ndims, gdims, dof, single);
}

}  // namespace IO
}  // namespace TIM
