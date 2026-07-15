// Gate 1b: blockDecomp / readReplicated round-trip through the real TIM::IO
// stack (File + DofMapCache + IoSystem). On 4 ranks (2x2 over an 8x6 grid):
//   1. write a known global field via the DOMAIN write path (File + Decomp2D);
//   2. read it back with File::readReplicated BELOW the threshold (the
//      broadcasting get_var branch) and ABOVE it (the block-decomposed
//      read_darray + MPI_Allgatherv branch, forced by a 1-byte threshold);
//   3. assert every rank holds the full, bitwise-correct global field in both.
//
//   CC --std=c++20 -I$PIO/include -o blockdecomp_roundtrip \
//      blockdecomp_roundtrip.cpp ../../tim/cpp/io/tim_file.cpp \
//      ../../tim/cpp/io/tim_iosystem.cpp ../../tim/cpp/io/tim_dofmap.cpp \
//      ../../tim/cpp/io/tim_backend.cpp -L$PIO/lib -lpioc $(nc-config --libs)
//   mpiexec -n 4 ./blockdecomp_roundtrip

#include "../../tim/cpp/core/tim_domain.hpp"
#include "../../tim/cpp/io/tim_file.hpp"
#include "../../tim/cpp/io/tim_iosystem.hpp"

#include <mpi.h>

#include <cstdio>
#include <vector>

using TIM::Decomp2D;
using TIM::Stagger;
using TIM::IO::File;
using TIM::IO::IoSystem;

static int rank, nprocs;
static const int NX = 8, NY = 6;

static double gval(int i, int j) { return 100.0 * (j + 1) + (i + 1); }

// 2x2 rank layout; rank r owns x block (r%2), y block (r/2). 1-based compute
// window (Decomp2D convention).
static void myWindow(int& is, int& ie, int& js, int& je) {
  const int px = rank % 2, py = rank / 2;
  const int bx = NX / 2, by = NY / 2;
  is = px * bx + 1; ie = is + bx - 1;
  js = py * by + 1; je = js + by - 1;
}

static int checkFull(const char* label, const std::vector<double>& out) {
  int local_ok = 1;
  for (int j = 0; j < NY && local_ok; ++j)
    for (int i = 0; i < NX; ++i)
      if (out[(size_t)j * NX + i] != gval(i, j)) { local_ok = 0; break; }
  int min_ok = 0;
  MPI_Allreduce(&local_ok, &min_ok, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
  if (rank == 0)
    std::printf("%s  %s (every rank holds the full correct field)\n",
                min_ok ? "PASS " : "FAIL ", label);
  return min_ok ? 0 : 1;
}

static void writeField(IoSystem& sys, const Decomp2D& d, const char* path) {
  auto f = File::create(sys, path, 0, d, File::Mode::Write);
  if (!f) { if (rank == 0) std::printf("FAIL create\n"); MPI_Abort(MPI_COMM_WORLD, 1); }
  f->defineAxis("xh", File::AxisKind::X, Stagger::Center, 0, "", "", "X", std::nullopt);
  f->defineAxis("yh", File::AxisKind::Y, Stagger::Center, 0, "", "", "Y", std::nullopt);
  f->defineVar("field", {"xh", "yh"}, "", "", "", false, "");
  int is, ie, js, je; myWindow(is, ie, js, je);
  const int wni = ie - is + 1, wnj = je - js + 1;
  std::vector<double> buf((size_t)wni * wnj);
  for (int j = js; j <= je; ++j)
    for (int i = is; i <= ie; ++i)
      buf[(size_t)(j - js) * wni + (i - is)] = gval(i - 1, j - 1);  // 0-based gval
  f->writeDecomposed("field", buf.data(), std::nullopt);
  f->close();
}

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  if (nprocs != 4) {
    if (rank == 0) std::fprintf(stderr, "run with exactly 4 ranks\n");
    MPI_Abort(MPI_COMM_WORLD, 2);
  }
  int is, ie, js, je; myWindow(is, ie, js, je);
  const Decomp2D dom(NX, NY, is, ie, js, je, false);

  const char* path = "spike_blockdecomp.nc";
  int rc = 0;

  if (rank == 0) std::printf("\n===== blockDecomp / readReplicated round-trip (4 ranks) =====\n");

  // ---- write once with a normal iosystem ----
  {
    IoSystem::Options o; o.niotasks = 2;
    IoSystem sys(MPI_COMM_WORLD, o);
    writeField(sys, dom, path);
  }

  // ---- small branch: default 8 MiB threshold => broadcasting get_var ----
  {
    IoSystem::Options o; o.niotasks = 2;  // default threshold (8 MiB)
    IoSystem sys(MPI_COMM_WORLD, o);
    auto f = File::openForRead(sys, path);
    if (!f) { if (rank == 0) std::printf("FAIL reopen small\n"); MPI_Abort(MPI_COMM_WORLD, 1); }
    std::vector<double> out((size_t)NX * NY, -1.0);
    f->readReplicated("field", 0, out.data());
    rc |= checkFull("small branch (get_var broadcast)", out);
  }

  // ---- large branch: 1-byte threshold => block read_darray + Allgatherv ----
  {
    IoSystem::Options o; o.niotasks = 2; o.replicated_read_threshold_bytes = 1;
    IoSystem sys(MPI_COMM_WORLD, o);
    auto f = File::openForRead(sys, path);
    if (!f) { if (rank == 0) std::printf("FAIL reopen large\n"); MPI_Abort(MPI_COMM_WORLD, 1); }
    std::vector<double> out((size_t)NX * NY, -1.0);
    f->readReplicated("field", 0, out.data());
    rc |= checkFull("large branch (block + Allgatherv)", out);
  }

  if (rank == 0) std::printf("=============================================================\n");
  MPI_Finalize();
  return rc;
}
