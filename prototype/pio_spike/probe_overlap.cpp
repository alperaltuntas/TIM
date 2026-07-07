// Probe: does PIO accept overlapping read maps, and via which API?
#include <pio.h>
#include <mpi.h>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  int rank, np;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &np);  // expect 4, 2x2 over 8x6 centers

  int iosysid;
  PIOc_Init_Intracomm(MPI_COMM_WORLD, 2, 2, 0, PIO_REARR_BOX, &iosysid);
  PIOc_set_iosystem_error_handling(iosysid, PIO_RETURN_ERROR, nullptr);

  const int NX = 8, NY = 6;
  const int px = rank % 2, py = rank / 2;
  const int isc = px * 4 + 1, iec = isc + 3, jsc = py * 3 + 1, jec = jsc + 2;

  auto makeMap = [&](bool overlap_x) {
    std::vector<PIO_Offset> dof;
    const int gnx = NX + (overlap_x ? 1 : 0);
    const int ie = overlap_x ? iec + 1 : iec;  // +1 on EVERY rank => overlap
    for (int j = jsc; j <= jec; ++j)
      for (int i = isc; i <= ie; ++i) dof.push_back((PIO_Offset)(j - 1) * gnx + i);
    return dof;
  };

  auto tryInit = [&](const char* name, bool ro, bool overlap) {
    auto dof = makeMap(overlap);
    int gdims[2] = {NY, NX + (overlap ? 1 : 0)};
    int ioid = -1, rearr = PIO_REARR_BOX;
    int rc = ro ? PIOc_InitDecomp_ReadOnly(iosysid, PIO_DOUBLE, 2, gdims,
                                           (int)dof.size(), dof.data(), &ioid,
                                           &rearr, nullptr, nullptr)
                : PIOc_InitDecomp(iosysid, PIO_DOUBLE, 2, gdims, (int)dof.size(),
                                  dof.data(), &ioid, &rearr, nullptr, nullptr);
    int minrc, maxrc;
    MPI_Allreduce(&rc, &minrc, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    MPI_Allreduce(&rc, &maxrc, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    if (rank == 0) std::printf("%-40s rc range [%d, %d]\n", name, minrc, maxrc);
    if (minrc == PIO_NOERR && maxrc == PIO_NOERR) PIOc_freedecomp(iosysid, ioid);
  };

  tryInit("regular  + disjoint", false, false);
  tryInit("ReadOnly + disjoint", true, false);
  tryInit("regular  + overlapping", false, true);
  tryInit("ReadOnly + overlapping", true, true);

  PIOc_finalize(iosysid);
  MPI_Finalize();
  return 0;
}
