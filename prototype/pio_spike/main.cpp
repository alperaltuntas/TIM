// PIO2 spike for the TIM diagnostics & I/O prototype (exit question 1).
// Deliberately tactical throwaway code: answers questions, nothing more.
//
// Questions probed here (4 MPI ranks, 2x2 layout over an 8x6 global grid):
//   Q1a  decomp construction: center vars, masked (holey) maps, a zero-maplen
//        rank, and symmetric-staggered corner vars (nx+1, ny+1)
//   Q1b  fill behavior for global cells owned by no rank
//   Q1c  PIO_RETURN_ERROR ergonomics (missing file, missing var)
//   Q1d  append: reopen an existing file, add a time frame
//   Q1e  iotype (PNETCDF vs NETCDF4P vs NETCDF-serial) x rearranger (BOX vs
//        SUBSET) roundtrip correctness
//
// Every test writes, reopens, reads back, and compares bitwise.

#include <pio.h>
#include <mpi.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int rank, nprocs;
static int iosysid = -1;

#define CHECK(rc, what)                                                        \
  do {                                                                         \
    if ((rc) != PIO_NOERR) {                                                   \
      std::fprintf(stderr, "[rank %d] FAIL rc=%d at %s (%s:%d)\n", rank, (rc), \
                   what, __FILE__, __LINE__);                                  \
      MPI_Abort(MPI_COMM_WORLD, 1);                                            \
    }                                                                          \
  } while (0)

struct Results {
  std::vector<std::string> lines;
  void add(const std::string& name, bool pass, const std::string& note = "") {
    lines.push_back((pass ? "PASS  " : "FAIL  ") + name +
                    (note.empty() ? "" : "   [" + note + "]"));
  }
};

static const int NX = 8, NY = 6;      // global center grid
static const double FILLV = -9.99e33;

// 2x2 rank layout; rank r owns x block (r%2), y block (r/2)
static void myBlock(int stagger_x, int stagger_y, int& is, int& ie, int& js,
                    int& je) {
  const int px = rank % 2, py = rank / 2;
  const int bx = NX / 2, by = NY / 2;
  is = px * bx;
  ie = is + bx - 1;
  js = py * by;
  je = js + by - 1;
  // symmetric staggering: easternmost/northernmost rank owns the extra col/row
  if (stagger_x && px == 1) ie += 1;
  if (stagger_y && py == 1) je += 1;
}

// Build a compdof map. mask==1 excludes cells where (i+j)%5==0 globally AND
// makes rank 3's map empty (zero-maplen rank, like an all-land PE).
static std::vector<PIO_Offset> makeMap(int stagger_x, int stagger_y, int mask) {
  const int gnx = NX + (stagger_x ? 1 : 0);
  std::vector<PIO_Offset> dof;
  if (mask && rank == 3) return dof;  // empty map on rank 3
  int is, ie, js, je;
  myBlock(stagger_x, stagger_y, is, ie, js, je);
  for (int j = js; j <= je; ++j)
    for (int i = is; i <= ie; ++i) {
      if (mask && ((i + j) % 5 == 0)) continue;  // global holes
      dof.push_back((PIO_Offset)j * gnx + i + 1); // 1-based
    }
  return dof;
}

static double expectedVal(PIO_Offset dof1based, int frame) {
  return 1000.0 * frame + (double)dof1based;
}

// Full roundtrip: init decomp, create file, write nframes, reopen, read back,
// bitwise compare. Returns true on success; fills note with details.
static bool roundtrip(int iotype, int rearr, int mask, int stagger,
                      int nframes, std::string& note) {
  const int gnx = NX + (stagger ? 1 : 0), gny = NY + (stagger ? 1 : 0);
  int gdims[2] = {gny, gnx};  // slowest-first for C API
  std::vector<PIO_Offset> dof = makeMap(stagger, stagger, mask);

  int ioid;
  // PIO rejects a NULL compmap with PIO_EINVAL even when maplen==0, so a
  // zero-cell rank (all-land PE) must still pass a valid pointer.
  static PIO_Offset dummy = 0;
  int rc = PIOc_InitDecomp(iosysid, PIO_DOUBLE, 2, gdims, (int)dof.size(),
                           dof.empty() ? &dummy : dof.data(), &ioid, &rearr,
                           nullptr, nullptr);
  CHECK(rc, "InitDecomp");

  char fname[128];
  std::snprintf(fname, sizeof fname, "spike_io%d_re%d_m%d_s%d.nc", iotype,
                rearr, mask, stagger);

  int ncid, dim_t, dim_y, dim_x, varid, tvar;
  rc = PIOc_createfile(iosysid, &ncid, &iotype, fname, PIO_CLOBBER);
  if (rc != PIO_NOERR) { note = "createfile rc=" + std::to_string(rc); return false; }
  CHECK(PIOc_def_dim(ncid, "time", PIO_UNLIMITED, &dim_t), "def_dim t");
  CHECK(PIOc_def_dim(ncid, "y", gny, &dim_y), "def_dim y");
  CHECK(PIOc_def_dim(ncid, "x", gnx, &dim_x), "def_dim x");
  int dims3[3] = {dim_t, dim_y, dim_x};
  CHECK(PIOc_def_var(ncid, "h", PIO_DOUBLE, 3, dims3, &varid), "def_var h");
  CHECK(PIOc_put_att_text(ncid, varid, "units", 1, "m"), "put_att");
  double fv = FILLV;
  CHECK(PIOc_put_att_double(ncid, varid, "_FillValue", PIO_DOUBLE, 1, &fv),
        "fill att");
  CHECK(PIOc_def_var(ncid, "time", PIO_DOUBLE, 1, dims3, &tvar), "def time");
  CHECK(PIOc_enddef(ncid), "enddef");

  std::vector<double> wbuf(dof.size());
  static double dummyd = 0.0;
  double* wptr = wbuf.empty() ? &dummyd : wbuf.data();
  for (int f = 0; f < nframes; ++f) {
    for (size_t k = 0; k < dof.size(); ++k) wbuf[k] = expectedVal(dof[k], f);
    CHECK(PIOc_setframe(ncid, varid, f), "setframe");
    rc = PIOc_write_darray(ncid, varid, ioid, (PIO_Offset)dof.size(),
                           wptr, &fv);
    if (rc != PIO_NOERR) { note = "write_darray rc=" + std::to_string(rc); return false; }
    PIO_Offset start = f, count = 1;
    double tval = (double)f;
    CHECK(PIOc_put_vara_double(ncid, tvar, &start, &count, &tval), "put time");
  }
  CHECK(PIOc_closefile(ncid), "close");

  // reopen + read_darray + bitwise compare
  rc = PIOc_openfile(iosysid, &ncid, &iotype, fname, PIO_NOWRITE);
  if (rc != PIO_NOERR) { note = "reopen rc=" + std::to_string(rc); return false; }
  CHECK(PIOc_inq_varid(ncid, "h", &varid), "inq_varid");
  std::vector<double> rbuf(dof.size(), 0.0);
  double* rptr = rbuf.empty() ? &dummyd : rbuf.data();
  for (int f = 0; f < nframes; ++f) {
    CHECK(PIOc_setframe(ncid, varid, f), "setframe r");
    rc = PIOc_read_darray(ncid, varid, ioid, (PIO_Offset)dof.size(), rptr);
    if (rc != PIO_NOERR) { note = "read_darray rc=" + std::to_string(rc); return false; }
    for (size_t k = 0; k < dof.size(); ++k)
      if (std::memcmp(&rbuf[k], &wbuf[k], sizeof(double)) != 0 &&
          expectedVal(dof[k], f) != rbuf[k]) {
        note = "mismatch frame " + std::to_string(f);
        return false;
      }
  }

  // masked case: verify hole cells hold the fill value (read full var, root
  // checks). PIOc_get_vara is collective; result valid on all tasks.
  if (mask) {
    std::vector<double> full((size_t)gnx * gny);
    PIO_Offset start[3] = {0, 0, 0}, count[3] = {1, (PIO_Offset)gny, (PIO_Offset)gnx};
    CHECK(PIOc_get_vara_double(ncid, varid, start, count, full.data()), "get full");
    for (int j = 0; j < gny; ++j)
      for (int i = 0; i < gnx; ++i) {
        const bool hole = ((i + j) % 5 == 0) ||
                          (i >= NX / 2 && j >= NY / 2);  // rank 3's block
        const double v = full[(size_t)j * gnx + i];
        if (hole && v != FILLV) { note = "hole not filled"; return false; }
        if (!hole && v == FILLV && i < NX && j < NY) { note = "data hole"; return false; }
      }
  }
  CHECK(PIOc_closefile(ncid), "close r");
  CHECK(PIOc_freedecomp(iosysid, ioid), "freedecomp");
  return true;
}

// Append a frame to an existing file written by a previous roundtrip.
static bool appendTest(int iotype, std::string& note) {
  int rearr = PIO_REARR_BOX;
  std::vector<PIO_Offset> dof = makeMap(0, 0, 0);
  int gdims[2] = {NY, NX};
  int ioid;
  CHECK(PIOc_InitDecomp(iosysid, PIO_DOUBLE, 2, gdims, (int)dof.size(),
                        dof.data(), &ioid, &rearr, nullptr, nullptr),
        "InitDecomp app");
  char fname[128];
  std::snprintf(fname, sizeof fname, "spike_io%d_re%d_m%d_s%d.nc", iotype,
                PIO_REARR_BOX, 0, 0);
  int ncid, varid, dim_t;
  int rc = PIOc_openfile(iosysid, &ncid, &iotype, fname, PIO_WRITE);
  if (rc != PIO_NOERR) { note = "open-for-append rc=" + std::to_string(rc); return false; }
  CHECK(PIOc_inq_varid(ncid, "h", &varid), "inq h");
  CHECK(PIOc_inq_dimid(ncid, "time", &dim_t), "inq time");
  PIO_Offset nrec;
  CHECK(PIOc_inq_dimlen(ncid, dim_t, &nrec), "inq dimlen");
  std::vector<double> wbuf(dof.size());
  for (size_t k = 0; k < dof.size(); ++k)
    wbuf[k] = expectedVal(dof[k], (int)nrec);
  double fv = FILLV;
  CHECK(PIOc_setframe(ncid, varid, (int)nrec), "setframe app");
  rc = PIOc_write_darray(ncid, varid, ioid, (PIO_Offset)dof.size(), wbuf.data(), &fv);
  if (rc != PIO_NOERR) { note = "append write rc=" + std::to_string(rc); return false; }
  CHECK(PIOc_closefile(ncid), "close app");

  // verify record count grew and data reads back
  CHECK(PIOc_openfile(iosysid, &ncid, &iotype, fname, PIO_NOWRITE), "reopen app");
  PIO_Offset nrec2;
  CHECK(PIOc_inq_dimid(ncid, "time", &dim_t), "inq t2");
  CHECK(PIOc_inq_dimlen(ncid, dim_t, &nrec2), "inq len2");
  if (nrec2 != nrec + 1) { note = "nrec did not grow"; return false; }
  std::vector<double> rbuf(dof.size());
  CHECK(PIOc_inq_varid(ncid, "h", &varid), "inq h2");
  CHECK(PIOc_setframe(ncid, varid, (int)nrec), "setframe app r");
  CHECK(PIOc_read_darray(ncid, varid, ioid, (PIO_Offset)dof.size(), rbuf.data()),
        "read app");
  for (size_t k = 0; k < dof.size(); ++k)
    if (rbuf[k] != wbuf[k]) { note = "append data mismatch"; return false; }
  CHECK(PIOc_closefile(ncid), "close app r");
  CHECK(PIOc_freedecomp(iosysid, ioid), "freedecomp app");
  return true;
}

// PIO_RETURN_ERROR ergonomics: do failures come back as return codes, with
// the library still usable afterward?
static bool errorModeTest(std::string& note) {
  int ncid, varid;
  int iotype = PIO_IOTYPE_NETCDF;
  int rc = PIOc_openfile(iosysid, &ncid, &iotype, "no_such_file_.nc", PIO_NOWRITE);
  if (rc == PIO_NOERR) { note = "missing file opened?!"; return false; }
  // library must remain usable:
  iotype = PIO_IOTYPE_PNETCDF;
  rc = PIOc_createfile(iosysid, &ncid, &iotype, "spike_errmode.nc", PIO_CLOBBER);
  if (rc != PIO_NOERR) { note = "create after error rc=" + std::to_string(rc); return false; }
  rc = PIOc_inq_varid(ncid, "nonexistent_var", &varid);
  if (rc == PIO_NOERR) { note = "missing var found?!"; return false; }
  CHECK(PIOc_enddef(ncid), "enddef err");
  CHECK(PIOc_closefile(ncid), "close err");
  note = "missing-file rc=" + std::to_string(rc);
  return true;
}

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  if (nprocs != 4) {
    if (rank == 0) std::fprintf(stderr, "run with exactly 4 ranks\n");
    MPI_Abort(MPI_COMM_WORLD, 2);
  }

  // 2 io tasks, stride 2
  CHECK(PIOc_Init_Intracomm(MPI_COMM_WORLD, 2, 2, 0, PIO_REARR_BOX, &iosysid),
        "Init_Intracomm");
  int old;
  CHECK(PIOc_set_iosystem_error_handling(iosysid, PIO_RETURN_ERROR, &old),
        "set error handling");

  Results res;
  std::string note;

  const int iotypes[] = {PIO_IOTYPE_PNETCDF, PIO_IOTYPE_NETCDF4P, PIO_IOTYPE_NETCDF};
  const char* ionames[] = {"PNETCDF", "NETCDF4P", "NETCDF-serial"};
  const int rearrs[] = {PIO_REARR_BOX, PIO_REARR_SUBSET};
  const char* renames[] = {"BOX", "SUBSET"};

  for (int it = 0; it < 3; ++it)
    for (int re = 0; re < 2; ++re) {
      note.clear();
      bool ok = roundtrip(iotypes[it], rearrs[re], 0, 0, 2, note);
      res.add(std::string("roundtrip center ") + ionames[it] + "/" + renames[re], ok, note);
    }
  for (int re = 0; re < 2; ++re) {
    note.clear();
    bool ok = roundtrip(PIO_IOTYPE_PNETCDF, rearrs[re], 1, 0, 2, note);
    res.add(std::string("masked+zero-maplen-rank PNETCDF/") + renames[re], ok, note);
  }
  note.clear();
  res.add("symmetric corner (nx+1,ny+1) PNETCDF/BOX",
          roundtrip(PIO_IOTYPE_PNETCDF, PIO_REARR_BOX, 0, 1, 2, note), note);
  note.clear();
  res.add("append frame PNETCDF", appendTest(PIO_IOTYPE_PNETCDF, note), note);
  note.clear();
  res.add("append frame NETCDF4P", appendTest(PIO_IOTYPE_NETCDF4P, note), note);
  note.clear();
  res.add("PIO_RETURN_ERROR ergonomics", errorModeTest(note), note);

  CHECK(PIOc_finalize(iosysid), "finalize");

  if (rank == 0) {
    std::printf("\n===== pio_spike results (%d ranks) =====\n", nprocs);
    for (auto& l : res.lines) std::printf("%s\n", l.c_str());
    std::printf("========================================\n");
  }
  MPI_Finalize();
  return 0;
}
