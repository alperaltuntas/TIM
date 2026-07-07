#include "tim_iosystem.hpp"

#include "tim_decomp_cache.hpp"

namespace TIM {
namespace IO {

int IoSystem::chooseIoTasks(int nprocs, const Options& opts) {
  // Sweep evidence (cesm_t232 restart reads, 768 ranks): 32 iotasks 1.32 s,
  // 96 3.72 s, 192 (=nprocs/4) 4.68 s, 8 5.48 s — too many iotasks is far
  // worse than too few. Cap the rank-based default; production should scale
  // this with data volume instead.
  int niotasks = opts.niotasks;
  if (niotasks <= 0) {
    niotasks = nprocs >= 4 ? nprocs / 4 : 1;
    if (niotasks > 64) niotasks = 64;
  }
  if (niotasks < 1) niotasks = 1;
  if (niotasks > nprocs) niotasks = nprocs;
  return niotasks;
}

IoSystem::IoSystem(MPI_Comm comm, const Options& opts) : comm_(comm) {
  int nprocs = 1;
  MPI_Comm_size(comm_, &nprocs);
  const int niotasks = chooseIoTasks(nprocs, opts);
  sys_ = Backend::init(comm_, niotasks, nprocs / niotasks);
  decomps_ = std::make_unique<DecompCache>(sys_);
}

IoSystem::~IoSystem() {
  decomps_.reset();  // decomps must be freed while the iosystem is alive
  if (sys_ >= 0) Backend::finalize(sys_);
}

}  // namespace IO
}  // namespace TIM
