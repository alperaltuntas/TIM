#pragma once
// TIM::IO::IoSystem — backend iosystem lifecycle + iotask policy (prototype
// design pass). A plain RAII object constructed with an EXPLICIT communicator:
// ensemble runs create one per member pelist. No global state; the owner
// (IoContext at the bind(C) boundary, or a test) controls the lifetime and
// must destroy it before MPI_Finalize.

#include "tim_backend.hpp"

#include <memory>

namespace TIM {
namespace IO {

class DofMapCache;

class IoSystem {
 public:
  struct Options {
    int niotasks = -1;  // -1: auto = min(nprocs/4, 64) (see sweep findings)
    // A non-domain (replicated) read at or above this size uses a block-
    // decomposed collective read + MPI_Allgatherv; smaller fields read once and
    // broadcast, because the collective+allgather latency loses to a tree bcast.
    long long replicated_read_threshold_bytes = 8LL << 20;  // 8 MiB
  };
  explicit IoSystem(MPI_Comm comm) : IoSystem(comm, Options{}) {}
  IoSystem(MPI_Comm comm, const Options& opts);
  ~IoSystem();
  IoSystem(const IoSystem&) = delete;
  IoSystem& operator=(const IoSystem&) = delete;

  SysId sys() const { return sys_; }
  MPI_Comm comm() const { return comm_; }
  DofMapCache& decomps() { return *decomps_; }
  long long replicatedReadThresholdBytes() const {
    return replicated_read_threshold_bytes_;
  }

 private:
  // Iotask policy, isolated for tuning; sweep evidence says cap the
  // rank-based default (production: scale with data volume instead).
  static int chooseIoTasks(int nprocs, const Options& opts);

  MPI_Comm comm_ = MPI_COMM_NULL;
  SysId sys_ = -1;
  long long replicated_read_threshold_bytes_ = 8LL << 20;
  std::unique_ptr<DofMapCache> decomps_;
};

}  // namespace IO
}  // namespace TIM
