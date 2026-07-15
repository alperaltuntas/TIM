#pragma once
// TIM::IO::DofMap / DofMapCache — the domain-agnostic decomposition core.
//
// A PIO decomposition is not a domain: PIOc_InitDecomp takes only global dims +
// a per-rank DOF list. This module owns every DOF list TIM builds and the
// backend decomposition it produces, behind three factories:
//   * fromDomain  — DOFs derived from a MOM Decomp2D compute window (the
//                   restart/history path). BIT-IDENTICAL to the retired
//                   DecompCache: the read-component / write-partition families,
//                   the k*gnx*gny + (j-1)*gnx + i formula, precision-keyed
//                   decomps, and the masked/symmetric edge handling are moved
//                   here verbatim, not rewritten.
//   * blockDecomp — a contiguous 1-D block partition of a flattened global
//                   array over all ranks of the iosystem communicator (no
//                   domain at all); the striped-collective replicated read.
// Every InitDecomp is collective and expensive, so the cache is the ONLY place
// DOF maps are created — ad-hoc maps must never proliferate.
//
// No pio.h here: DofMap carries an opaque backend id (an int today), exactly
// like the rest of TIM::IO. tim_backend.* stays the sole ParallelIO includer.

#include "../core/tim_domain.hpp"
#include "tim_backend.hpp"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace TIM {
namespace IO {

// Opaque handle to one backend decomposition plus the local element count that
// read_darray / write_darray expect for it (0 on an all-land / empty rank).
class DofMap {
 public:
  DofMap() = default;
  bool valid() const { return id_ >= 0; }
  DecompId id() const { return id_; }        // opaque; -1 when default-built
  long long count() const { return count_; }  // local dof count

 private:
  friend class DofMapCache;
  DofMap(DecompId id, long long count) : id_(id), count_(count) {}
  DecompId id_ = -1;
  long long count_ = 0;
};

class DofMapCache {
 public:
  // Read fetches a staggered window as up to 4 disjoint components (PIO forbids
  // overlapping maps); writes lay down one true partition (every global point
  // once). Same two families the prototype DecompCache carried.
  enum class Family : int { ReadComponent = 0, WritePartition = 1 };

  DofMapCache(SysId sys, MPI_Comm comm) : sys_(sys), comm_(comm) {}
  ~DofMapCache();
  DofMapCache(const DofMapCache&) = delete;
  DofMapCache& operator=(const DofMapCache&) = delete;

  // Domain factory. `domainKey` identifies the Decomp2D (registry handle); the
  // Decomp2D supplies the geometry. `fam`/`comp` pick the read component or the
  // write partition; nz is total depth, nz2 the 4th-dim extent; single selects
  // a float basetype (float vars need type-matched decomps). Cached on
  // (domainKey, stagger, fam, comp, single, nz2, nz) — the prototype key,
  // unchanged, so the DOFs (and thus the rearranger schedule and the bytes) are
  // bit-identical to the pre-refactor decomp cache.
  const DofMap& fromDomain(int domainKey, const Decomp2D& d, Stagger stagger,
                           int nz, int nz2, Family fam, int comp = 0,
                           bool single = false);

  // Block factory. A contiguous 1-D block partition of the flattened global
  // array (gdims slowest-first, x fastest, matching read_darray) over every
  // rank of the iosystem communicator: rank r owns elements
  // [r*chunk, min((r+1)*chunk, n)), chunk = ceil(n / nranks), 1-based DOFs.
  // Cached on (gdims, nranks, single).
  const DofMap& blockDecomp(std::span<const int> gdims, bool single = false);

  // Block partition of a single flat sub-range [base, base+len) of the global
  // array described by (gdims, ndims). Used by replicated 3-D reads that must
  // stripe one z-level at a time to keep MPI_Allgatherv int displacements in
  // range. Cached on (gdims, base, len, nranks, single).
  const DofMap& blockDecompRange(std::span<const int> gdims, long long len,
                                 long long base, bool single = false);

  MPI_Comm comm() const { return comm_; }

  // The pure DOF-list generator behind fromDomain (no backend, no PIO): the
  // 1-based, x-fastest, Fortran-order flat offsets k*gnx*gny + (j-1)*gnx + i for
  // the window `w` at `stagger` over nz levels. Exposed so the gate-1c identity
  // test can prove it is bit-identical to the retired DecompCache.
  static std::vector<long long> domainDofs(const Decomp2D& d, Stagger stagger,
                                           const Window& w, int nz);

 private:
  const DofMap& emplaceDomain(long long key, const Decomp2D& d, Stagger stagger,
                              const Window& w, int nz, int nz2, bool single);
  const DofMap& buildBlock(const std::string& key, const int* gdims, int ndims,
                           long long len, long long base, bool single);

  SysId sys_;
  MPI_Comm comm_;
  std::map<long long, DofMap> domain_cache_;
  std::map<std::string, DofMap> block_cache_;
};

}  // namespace IO
}  // namespace TIM
