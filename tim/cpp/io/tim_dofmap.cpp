#include "tim_dofmap.hpp"

#include <algorithm>

namespace TIM {
namespace IO {

DofMapCache::~DofMapCache() {
  // Backend decompositions die with the iosystem finalize; freeing each here
  // needs the iosystem still alive, which IoSystem guarantees by destroying
  // this cache first.
  for (auto& kv : domain_cache_) Backend::freeDecomp(sys_, kv.second.id());
  for (auto& kv : block_cache_) Backend::freeDecomp(sys_, kv.second.id());
}

// ---- domain factory (verbatim DOF generation from the retired DecompCache) --

const DofMap& DofMapCache::fromDomain(int domainKey, const Decomp2D& d,
                                      Stagger stagger, int nz, int nz2,
                                      Family fam, int comp, bool single) {
  const long long key = ((long long)domainKey << 48) |
                        ((long long)(int)stagger << 44) |
                        ((long long)(int)fam << 40) | ((long long)comp << 36) |
                        ((long long)(single ? 1 : 0) << 35) |
                        ((long long)nz2 << 20) | nz;
  auto it = domain_cache_.find(key);
  if (it != domain_cache_.end()) return it->second;

  Window w;
  if (fam == Family::WritePartition) {
    w = d.writePartition(stagger);
  } else {
    Window comps[4];
    const int n = d.readComponents(stagger, comps);
    w = (comp < n) ? comps[comp] : Window{};  // empty window if absent
  }
  return emplaceDomain(key, d, stagger, w, nz, nz2, single);
}

std::vector<long long> DofMapCache::domainDofs(const Decomp2D& d,
                                               Stagger stagger, const Window& w,
                                               int nz) {
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

const DofMap& DofMapCache::emplaceDomain(long long key, const Decomp2D& d,
                                         Stagger stagger, const Window& w,
                                         int nz, int nz2, bool single) {
  const int gnx = d.globalNx(stagger), gny = d.globalNy(stagger);

  std::vector<long long> dof = domainDofs(d, stagger, w, nz);

  // The backend needs the decomp's rank to match the variable's non-record
  // rank; a 4-d variable (x,y,z1,z2 in Fortran order) shares the flat index of
  // the flattened nz = nz1*nz2 3-d case.
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
  const DecompId id = Backend::initDecomp(sys_, ndims, gdims, dof, single);
  auto res = domain_cache_.emplace(key, DofMap(id, (long long)dof.size()));
  return res.first->second;
}

// ---- block factories (no domain: contiguous partition of a flat array) ------

const DofMap& DofMapCache::blockDecomp(std::span<const int> gdims, bool single) {
  long long n = 1;
  for (int g : gdims) n *= g;
  return blockDecompRange(gdims, n, 0, single);
}

const DofMap& DofMapCache::blockDecompRange(std::span<const int> gdims,
                                            long long len, long long base,
                                            bool single) {
  int nranks = 1;
  MPI_Comm_size(comm_, &nranks);
  std::string key;
  key.reserve(gdims.size() * 8 + 32);
  for (int g : gdims) { key += std::to_string(g); key += ','; }
  key += 'L'; key += std::to_string(len);
  key += 'B'; key += std::to_string(base);
  key += 'N'; key += std::to_string(nranks);
  key += single ? 'f' : 'd';
  auto it = block_cache_.find(key);
  if (it != block_cache_.end()) return it->second;
  return buildBlock(key, gdims.data(), (int)gdims.size(), len, base, single);
}

const DofMap& DofMapCache::buildBlock(const std::string& key, const int* gdims,
                                      int ndims, long long len, long long base,
                                      bool single) {
  int nranks = 1, myrank = 0;
  MPI_Comm_size(comm_, &nranks);
  MPI_Comm_rank(comm_, &myrank);
  const long long chunk = (len + nranks - 1) / nranks;  // ceil
  const long long s = std::min((long long)myrank * chunk, len);
  const long long e = std::min((long long)(myrank + 1) * chunk, len);

  std::vector<long long> dof;
  dof.reserve((size_t)(e - s));
  for (long long g = s; g < e; ++g) dof.push_back(base + g + 1);  // 1-based

  const DecompId id = Backend::initDecomp(sys_, ndims, gdims, dof, single);
  auto res = block_cache_.emplace(key, DofMap(id, (long long)dof.size()));
  return res.first->second;
}

}  // namespace IO
}  // namespace TIM
