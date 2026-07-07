// Standalone TIM::IO read test on the tx1_12 (4320x3240x75) split restart set.
// Validates multi-file / 8-GB CDF-2 / staggered reads at 128 ranks without the
// model's memory footprint, and times the TIM read path alone.
#include "../../tim/cpp/io/tim_io.hpp"
#include <mpi.h>
#include <pio.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  int rank, np;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &np);

  const int NIG = 4320, NJG = 3240, NK = 75;
  const int PX = 16, PY = np / 16;  // uniform block layout
  const int px = rank % PX, py = rank / PX;
  TIM::IO::DomainInfo d;
  d.nig = NIG; d.njg = NJG; d.symmetric = 1;
  d.isc = px * (NIG / PX) + 1; d.iec = d.isc + NIG / PX - 1;
  d.jsc = py * (NJG / PY) + 1; d.jec = d.jsc + NJG / PY - 1;
  int h = TIM::IO::registerDomain(d);

  const char* base = argc > 1 ? argv[1] : ".";
  struct Item { const char* file; const char* var; int stag; };
  const Item items[] = {{"MOM.res.nc", "Temp", 0}, {"MOM.res_1.nc", "Salt", 0},
                        {"MOM.res_2.nc", "h", 0},  {"MOM.res_3.nc", "u", 1},
                        {"MOM.res_4.nc", "v", 2}};
  // window buffer: +1 in each dir is enough for any stagger
  std::vector<double> buf((size_t)(NIG / PX + 1) * (NJG / PY + 1) * NK);

  MPI_Barrier(MPI_COMM_WORLD);
  double t0 = MPI_Wtime();
  for (const auto& it : items) {
    double ti = MPI_Wtime();
    int fsx = 0, fsy = 0;
    int rc = TIM::IO::readDecomposed(std::string(base) + "/" + it.file, it.var,
                                     h, it.stag, 1, NK, 1, buf.data(), &fsx,
                                     &fsy);
    MPI_Barrier(MPI_COMM_WORLD);

    // Value spot-check: read a small k=0 slab at rank 0's window origin via
    // collective get_vara (all ranks receive it) and compare with the
    // scattered buffer on rank 0. Buffer x-index of file point i (1-based)
    // for rank 0 is (i-1)+shx with shx = want_stagger_x - fsx; row shy.
    const int NCHK = 8;
    std::vector<double> slab(NCHK, -1.0);
    {
      int ncid, varid, iotype = 1 /*PIO_IOTYPE_PNETCDF*/;
      PIOc_openfile(TIM::IO::iosysId(), &ncid, &iotype,
                    (std::string(base) + "/" + it.file).c_str(), PIO_NOWRITE);
      PIOc_inq_varid(ncid, it.var, &varid);
      PIO_Offset start[4] = {0, 0, 0, 0}, count[4] = {1, 1, 1, NCHK};
      PIOc_get_vara_double(ncid, varid, start, count, slab.data());
      PIOc_closefile(ncid);
    }
    bool ok = true;
    if (rank == 0) {
      const bool wsx = (it.stag == 1 || it.stag == 3);
      const bool wsy = (it.stag == 2 || it.stag == 3);
      const int shx = (wsx ? 1 : 0) - fsx, shy = (wsy ? 1 : 0) - fsy;
      const int wni = NIG / PX + (wsx ? 1 : 0);
      for (int m = 0; m < NCHK; ++m) {
        const double a = buf[(size_t)shy * wni + m + shx], b = slab[m];
        if (a != b && !(std::isnan(a) && std::isnan(b))) ok = false;
      }
    }
    if (rc != 0) { MPI_Abort(MPI_COMM_WORLD, 1); }
    if (rank == 0)
      std::printf("%-14s %-5s stag=%d rc=%d fsx=%d fsy=%d values=%s  %.2f s\n",
                  it.file, it.var, it.stag, rc, fsx, fsy, ok ? "OK" : "BAD",
                  MPI_Wtime() - ti);
  }
  if (rank == 0)
    std::printf("TOTAL: 5 x 8.4 GB vars (42 GB) in %.2f s => %.2f GB/s\n",
                MPI_Wtime() - t0, 42.0 / (MPI_Wtime() - t0));
  TIM::IO::finalize();
  MPI_Finalize();
  return 0;
}
