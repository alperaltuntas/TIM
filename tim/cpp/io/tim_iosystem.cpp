#include "tim_iosystem.hpp"

#include "tim_decomp_cache.hpp"

#include <mpi.h>

#include <cstdlib>

namespace TIM {
namespace IO {

static IoSystem* g_instance = nullptr;

IoSystem& IoSystem::instance() {
  if (!g_instance) g_instance = new IoSystem();
  return *g_instance;
}

void IoSystem::shutdown() {
  delete g_instance;
  g_instance = nullptr;
}

int IoSystem::chooseIoTasks(int nprocs) {
  int niotasks = nprocs >= 4 ? nprocs / 4 : 1;
  if (const char* s = std::getenv("TIM_PIO_NTASKS")) niotasks = std::atoi(s);
  if (niotasks < 1) niotasks = 1;
  if (niotasks > nprocs) niotasks = nprocs;
  return niotasks;
}

IoSystem::IoSystem() {
  int nprocs = 1;
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  const int niotasks = chooseIoTasks(nprocs);
  sys_ = Backend::init(0, niotasks, nprocs / niotasks);
  decomps_ = new DecompCache(sys_);
}

IoSystem::~IoSystem() {
  delete decomps_;
  if (sys_ >= 0) Backend::finalize(sys_);
}

}  // namespace IO
}  // namespace TIM
