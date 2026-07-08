#pragma once
// Internal cross-adapter access: the diag C-API adapter borrows the io
// adapter's IoContext (same component, same communicator). NOT part of the
// extern "C" surface; C++ TUs only.

namespace TIM {
namespace IO {
class IoContext;

// The component's IoContext (created by tim_io_init); creates a
// MPI_COMM_WORLD fallback context if tim_io_init was skipped.
IoContext& currentIoContext();

}  // namespace IO
}  // namespace TIM
