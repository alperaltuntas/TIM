#pragma once
// TIM::IO::IoSystem — backend iosystem lifecycle + iotask policy (prototype
// design pass). One instance per communicator; prototype has exactly one
// (MPI_COMM_WORLD). Deliberately an explicit-lifetime singleton rather than
// RAII: the iosystem must outlive every File and die before MPI_Finalize,
// a lifetime no scoped object owns naturally in a Fortran-driven program.

#include "tim_backend.hpp"

namespace TIM {
namespace IO {

class DecompCache;

class IoSystem {
 public:
  static IoSystem& instance();   // initializes the backend on first use
  static void shutdown();        // idempotent; finalizes the backend

  SysId sys() const { return sys_; }
  DecompCache& decomps() { return *decomps_; }

  IoSystem(const IoSystem&) = delete;
  IoSystem& operator=(const IoSystem&) = delete;

 private:
  IoSystem();
  ~IoSystem();

  // Iotask policy, isolated for tuning: currently nprocs/4 with env override
  // (TIM_PIO_NTASKS). Finding pending from the 768-rank sweep: the count
  // should scale with data volume, not rank count.
  static int chooseIoTasks(int nprocs);

  SysId sys_ = -1;
  DecompCache* decomps_ = nullptr;
};

}  // namespace IO
}  // namespace TIM
