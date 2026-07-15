#include "tim_iosystem.hpp"

#include "tim_dofmap.hpp"

namespace TIM {
namespace IO {

int IoSystem::chooseIoTasks(int nprocs, const Options& opts) {
  // Sweep evidence (cesm_t232 restart reads): 32 iotasks was the sweet spot at
  // BOTH 128 and 768 ranks (768: 32->1.32s best, 96->3.72s, 192(=nprocs/4)->
  // 4.68s, 8->5.48s). The optimum is ~constant, not proportional to nprocs —
  // I/O-aggregation bandwidth saturates — so the default is a fixed target,
  // capped at nprocs for small jobs. Override per run with tim.io.pio_ntasks
  // / TIM_PIO_NTASKS; very large runs should retune with a widened sweep.
  int niotasks = opts.niotasks;
  if (niotasks <= 0) niotasks = 32;
  if (niotasks < 1) niotasks = 1;
  if (niotasks > nprocs) niotasks = nprocs;
  return niotasks;
}

IoSystem::IoSystem(MPI_Comm comm, const Options& opts)
    : comm_(comm),
      replicated_read_threshold_bytes_(opts.replicated_read_threshold_bytes) {
  int nprocs = 1;
  MPI_Comm_size(comm_, &nprocs);
  const int niotasks = chooseIoTasks(nprocs, opts);
  sys_ = Backend::init(comm_, niotasks, nprocs / niotasks);
  decomps_ = std::make_unique<DofMapCache>(sys_, comm_);
}

IoSystem::~IoSystem() {
  decomps_.reset();  // decomps must be freed while the iosystem is alive
  if (sys_ >= 0) Backend::finalize(sys_);
}

}  // namespace IO
}  // namespace TIM
