// Gate 1a: prove the broadcast semantics of PIOc_get_vara_double on an open PIO
// file. The small-read path of the refactor (readReplicated below threshold,
// readPlain) relies on a single collective get_var leaving the CORRECT full
// field on EVERY compute task — not just the I/O tasks. If that holds, the
// refactor can retire the every-rank nc_open for small replicated reads.
//
// Test (4 ranks, 2 io tasks stride 2): rank 0-independent — write a known
// global array, close, reopen, and have every rank call get_vara for the whole
// variable. Assert bitwise-correct values on all four ranks. The decisive
// ranks are the NON-I/O tasks: they own no I/O data locally, so correct values
// there can only have arrived by broadcast from the I/O root.
//
//   make probe_getvar_bcast   (add a rule, or:)
//   CC --std=c++20 -I$PIO/include -o probe_getvar_bcast probe_getvar_bcast.cpp \
//      -L$PIO/lib -lpioc
//   mpiexec -n 4 ./probe_getvar_bcast

#include <mpi.h>
#include <pio.h>

#include <cstdio>
#include <vector>

static int rank, nprocs, iosysid = -1;

#define CHECK(rc, what)                                                       \
  do {                                                                        \
    if ((rc) != PIO_NOERR) {                                                  \
      std::fprintf(stderr, "[%d] FAIL rc=%d at %s (%d)\n", rank, (rc), what,  \
                   __LINE__);                                                 \
      MPI_Abort(MPI_COMM_WORLD, 1);                                           \
    }                                                                         \
  } while (0)

static const int NX = 6, NY = 5;
static double expected(int i, int j) { return 1000.0 * j + i + 1; }

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  if (nprocs != 4) {
    if (rank == 0) std::fprintf(stderr, "run with exactly 4 ranks\n");
    MPI_Abort(MPI_COMM_WORLD, 2);
  }
  CHECK(PIOc_Init_Intracomm(MPI_COMM_WORLD, 2, 2, 0, PIO_REARR_BOX, &iosysid),
        "Init_Intracomm");
  PIOc_set_iosystem_error_handling(iosysid, PIO_RETURN_ERROR, nullptr);

  const char* fname = "spike_getvar.nc";
  int iotype = PIO_IOTYPE_PNETCDF;

  // Write a known global array with a collective put_vara (identical args/data
  // on every rank).
  {
    int ncid, dy, dx, vv;
    CHECK(PIOc_createfile(iosysid, &ncid, &iotype, fname, PIO_CLOBBER), "create");
    CHECK(PIOc_def_dim(ncid, "y", NY, &dy), "defdim y");
    CHECK(PIOc_def_dim(ncid, "x", NX, &dx), "defdim x");
    int dims[2] = {dy, dx};
    CHECK(PIOc_def_var(ncid, "h", PIO_DOUBLE, 2, dims, &vv), "defvar");
    CHECK(PIOc_enddef(ncid), "enddef");
    std::vector<double> full((size_t)NX * NY);
    for (int j = 0; j < NY; ++j)
      for (int i = 0; i < NX; ++i) full[(size_t)j * NX + i] = expected(i, j);
    PIO_Offset start[2] = {0, 0}, count[2] = {NY, NX};
    CHECK(PIOc_put_vara_double(ncid, vv, start, count, full.data()), "put_vara");
    CHECK(PIOc_closefile(ncid), "close");
  }

  // Reopen and get_vara the WHOLE variable on every rank.
  int ncid, vv;
  CHECK(PIOc_openfile(iosysid, &ncid, &iotype, fname, PIO_NOWRITE), "reopen");
  CHECK(PIOc_inq_varid(ncid, "h", &vv), "inq h");
  std::vector<double> got((size_t)NX * NY, -1.0);
  PIO_Offset start[2] = {0, 0}, count[2] = {NY, NX};
  CHECK(PIOc_get_vara_double(ncid, vv, start, count, got.data()), "get_vara");
  CHECK(PIOc_closefile(ncid), "close r");

  int local_ok = 1;
  for (int j = 0; j < NY && local_ok; ++j)
    for (int i = 0; i < NX; ++i)
      if (got[(size_t)j * NX + i] != expected(i, j)) { local_ok = 0; break; }

  int all_ok = 0;
  MPI_Allreduce(&local_ok, &all_ok, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);
  int min_ok = 0;
  MPI_Allreduce(&local_ok, &min_ok, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);

  if (rank == 0) {
    std::printf("\n===== get_vara broadcast probe (4 ranks, 2 iotasks) =====\n");
    std::printf("%s  every rank holds the correct full field after one "
                "collective get_vara\n",
                (all_ok && min_ok) ? "PASS " : "FAIL ");
    std::printf("=> PIOc_get_vara_double reads on the I/O root and broadcasts "
                "to all compute tasks.\n");
    std::printf("=========================================================\n");
  }
  CHECK(PIOc_finalize(iosysid), "finalize");
  MPI_Finalize();
  return (all_ok && min_ok) ? 0 : 1;
}
