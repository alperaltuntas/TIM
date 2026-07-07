// Standalone TIM::IO read test on the tx1_12 (4320x3240x75) split restart set
// (redesigned File API). Validates multi-file / 8-GB CDF-2 / staggered reads
// at scale without the model's memory footprint; times the TIM path alone.
#include "../../tim/cpp/core/tim_domain.hpp"
#include "../../tim/cpp/io/tim_file.hpp"
#include "../../tim/cpp/io/tim_iosystem.hpp"
#include <mpi.h>
#include <cstdio>
#include <string>
#include <vector>

using TIM::Decomp2D;
using TIM::Stagger;
using TIM::IO::File;

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  int rank, np;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &np);

  const int NIG = 4320, NJG = 3240, NK = 75;
  const int PX = 16, PY = np / 16;
  const int px = rank % PX, py = rank / PX;
  Decomp2D d(NIG, NJG, px * (NIG / PX) + 1, (px + 1) * (NIG / PX),
             py * (NJG / PY) + 1, (py + 1) * (NJG / PY), true);

  const char* base = argc > 1 ? argv[1] : ".";
  struct Item { const char* file; const char* var; Stagger stag; };
  const Item items[] = {{"MOM.res.nc", "Temp", Stagger::Center},
                        {"MOM.res_1.nc", "Salt", Stagger::Center},
                        {"MOM.res_2.nc", "h", Stagger::Center},
                        {"MOM.res_3.nc", "u", Stagger::EastFace},
                        {"MOM.res_4.nc", "v", Stagger::NorthFace}};
  std::vector<double> buf((size_t)(NIG / PX + 1) * (NJG / PY + 1) * NK);

  MPI_Barrier(MPI_COMM_WORLD);
  double t0 = MPI_Wtime();
  for (const auto& it : items) {
    double ti = MPI_Wtime();
    auto f = File::openForRead(std::string(base) + "/" + it.file);
    if (!f) { if (rank == 0) std::printf("OPEN FAILED %s\n", it.file); MPI_Abort(MPI_COMM_WORLD, 1); }
    File::ReadInfo info;
    int rc = f->readDecomposed(it.var, 0, d, it.stag, 1, NK, 1, buf.data(), &info);
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0)
      std::printf("%-14s %-5s stag=%d rc=%d fsx=%d fsy=%d  %.2f s\n", it.file,
                  it.var, (int)it.stag, rc, info.file_sx, info.file_sy,
                  MPI_Wtime() - ti);
    if (rc != 0) MPI_Abort(MPI_COMM_WORLD, 1);
  }
  if (rank == 0)
    std::printf("TOTAL: 5 x 8.4 GB vars (42 GB) in %.2f s => %.2f GB/s\n",
                MPI_Wtime() - t0, 42.0 / (MPI_Wtime() - t0));
  TIM::IO::IoSystem::shutdown();
  MPI_Finalize();
  return 0;
}
