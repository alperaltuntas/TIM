// PROTOTYPE (throwaway) TIM I/O spine over ParallelIO — read path.
// See tim_io.hpp. Tactical: minimal abstraction, maximal learning.

#include "tim_io.hpp"
#include "tim_io_C_API.h"

#include <mpi.h>
#include <pio.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace TIM {
namespace IO {

static int iosysid = -1;
static std::vector<DomainInfo> domains;

// (domain_handle, stagger, nz) -> ioid; tactical decomp cache
static std::map<long long, int> decomp_cache;

static bool amRoot() {
  int r; MPI_Comm_rank(MPI_COMM_WORLD, &r); return r == 0;
}

static bool debugOn() {
  static int on = -1;
  if (on < 0) { const char* s = std::getenv("TIM_IO_DEBUG"); on = (s && s[0] == '1') ? 1 : 0; }
  return on == 1;
}

static void ensureInit() {
  if (iosysid >= 0) return;
  int nprocs;
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  // PROTOTYPE ASSUMPTION: compute pelist == MPI_COMM_WORLD (standalone MOM6).
  // Production must take the component communicator (ensembles!).
  int niotasks = nprocs >= 4 ? nprocs / 4 : 1;
  if (const char* s = std::getenv("TIM_PIO_NTASKS")) niotasks = std::atoi(s);
  if (niotasks < 1) niotasks = 1;
  if (niotasks > nprocs) niotasks = nprocs;
  int stride = nprocs / niotasks;
  int rc = PIOc_Init_Intracomm(MPI_COMM_WORLD, niotasks, stride, 0,
                               PIO_REARR_BOX, &iosysid);
  if (rc != PIO_NOERR) {
    std::fprintf(stderr, "tim_io: PIOc_Init_Intracomm failed rc=%d\n", rc);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  PIOc_set_iosystem_error_handling(iosysid, PIO_RETURN_ERROR, nullptr);
}

int registerDomain(const DomainInfo& d) {
  ensureInit();
  domains.push_back(d);
  return (int)domains.size() - 1;
}

// Staggered read windows (1-based file indices). FMS symmetric convention:
// the staggered axis has nig+1 points; file index p corresponds to MOM global
// staggered index I = p-1; every rank's staggered compute window is
// [isc, iec+1] x [jsc, jec+1] — windows overlap at shared edges (that is how
// FMS fills each rank's full window with no post-read halo update).
//
// PIO cannot express overlap: the BOX rearranger heap-corrupts on duplicated
// map entries and InitDecomp{,_ReadOnly} returns PIO_EINVAL (verified,
// prototype/pio_spike/probe_overlap.cpp). So each staggered window is read as
// up to FOUR strictly disjoint pieces, each its own cached decomposition:
//   comp 0: [isc..iec] x [jsc..jec]   (main block; all vars)
//   comp 1: [iec+1]    x [jsc..jec]   (east strip; sx only)
//   comp 2: [isc..iec] x [jec+1]      (north strip; sy only)
//   comp 3: [iec+1]    x [jec+1]      (NE point; sx && sy)
// Each strip is disjoint across ranks because iec+1 == the east neighbor's
// isc, which that neighbor never reads in comp 1 (it reads its own iec+1).
static void staggeredExtents(const DomainInfo& d, int stagger, int& gnx,
                             int& gny, int& is, int& ie, int& js, int& je) {
  const bool sx = (stagger == EAST_FACE || stagger == CORNER) && d.symmetric;
  const bool sy = (stagger == NORTH_FACE || stagger == CORNER) && d.symmetric;
  gnx = d.nig + (sx ? 1 : 0);
  gny = d.njg + (sy ? 1 : 0);
  is = d.isc;
  ie = d.iec + (sx ? 1 : 0);
  js = d.jsc;
  je = d.jec + (sy ? 1 : 0);
}

// Window of one disjoint component (see table above). Returns false if the
// component is empty for this stagger.
static bool componentWindow(const DomainInfo& d, int stagger, int comp,
                            int& is, int& ie, int& js, int& je) {
  const bool sx = (stagger == EAST_FACE || stagger == CORNER) && d.symmetric;
  const bool sy = (stagger == NORTH_FACE || stagger == CORNER) && d.symmetric;
  switch (comp) {
    case 0: is = d.isc; ie = d.iec; js = d.jsc; je = d.jec; return true;
    case 1: if (!sx) return false;
            is = ie = d.iec + 1; js = d.jsc; je = d.jec; return true;
    case 2: if (!sy) return false;
            is = d.isc; ie = d.iec; js = je = d.jec + 1; return true;
    case 3: if (!(sx && sy)) return false;
            is = ie = d.iec + 1; js = je = d.jec + 1; return true;
  }
  return false;
}

// nz = total trailing (non-horizontal) extent = nz1*nz2; nz2 > 1 means the
// file variable is 4-d (x,y,z1,z2 in Fortran order => t,z2,z1,y,x on file);
// the flat file index is identical either way, but PIO wants the decomp's
// ndims to match the variable, so the gdims list is built accordingly.
static int getDecomp(int domain_handle, int stagger, int nz, int nz2, int comp) {
  const long long key = ((long long)domain_handle << 44) |
                        ((long long)stagger << 40) | ((long long)comp << 36) |
                        ((long long)nz2 << 20) | nz;
  auto it = decomp_cache.find(key);
  if (it != decomp_cache.end()) return it->second;

  const DomainInfo& d = domains[(size_t)domain_handle];
  int gnx, gny, ws, we, wjs, wje;
  staggeredExtents(d, stagger, gnx, gny, ws, we, wjs, wje);
  int is, ie, js, je;
  if (!componentWindow(d, stagger, comp, is, ie, js, je))
    MPI_Abort(MPI_COMM_WORLD, 1);

  std::vector<PIO_Offset> dof;
  dof.reserve((size_t)(ie - is + 1) * (je - js + 1) * nz);
  for (int k = 0; k < nz; ++k)
    for (int j = js; j <= je; ++j)
      for (int i = is; i <= ie; ++i)
        dof.push_back((PIO_Offset)k * gnx * gny + (PIO_Offset)(j - 1) * gnx + i);

  const int nz1 = (nz2 > 1) ? nz / nz2 : nz;
  int gdims[4] = {gny, gnx, 0, 0};
  int ndims = 2;
  if (nz2 > 1) {
    ndims = 4; gdims[0] = nz2; gdims[1] = nz1; gdims[2] = gny; gdims[3] = gnx;
  } else if (nz > 1) {
    ndims = 3; gdims[0] = nz; gdims[1] = gny; gdims[2] = gnx;
  }
  static PIO_Offset dummy = 0;  // zero-maplen ranks need a non-null pointer
  int ioid, rearr = PIO_REARR_BOX;
  int rc = PIOc_InitDecomp(iosysid, PIO_DOUBLE, ndims, gdims, (int)dof.size(),
                           dof.empty() ? &dummy : dof.data(), &ioid, &rearr,
                           nullptr, nullptr);
  if (rc != PIO_NOERR) {
    std::fprintf(stderr, "tim_io: InitDecomp failed rc=%d\n", rc);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  decomp_cache[key] = ioid;
  return ioid;
}

static bool ciEqual(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
      return false;
  return *a == *b;
}

// open + case-insensitive var lookup. Returns rc; on success fills ncid/varid.
static int openAndFind(const std::string& path, const std::string& varname,
                       int* ncid, int* varid, bool* has_unlim_dim) {
  ensureInit();
  int iotype = PIO_IOTYPE_PNETCDF;
  int rc = PIOc_openfile(iosysid, ncid, &iotype, path.c_str(), PIO_NOWRITE);
  if (rc != PIO_NOERR) {  // netCDF-4/HDF5 files need the other iotype
    iotype = PIO_IOTYPE_NETCDF4P;
    rc = PIOc_openfile(iosysid, ncid, &iotype, path.c_str(), PIO_NOWRITE);
  }
  if (rc != PIO_NOERR) return rc;

  rc = PIOc_inq_varid(*ncid, varname.c_str(), varid);
  if (rc != PIO_NOERR) {  // case-insensitive fallback
    int nvars = 0;
    PIOc_inq_nvars(*ncid, &nvars);
    char name[PIO_MAX_NAME + 1];
    for (int v = 0; v < nvars; ++v) {
      PIOc_inq_varname(*ncid, v, name);
      if (ciEqual(name, varname.c_str())) { *varid = v; rc = PIO_NOERR; break; }
    }
    if (rc != PIO_NOERR) { PIOc_closefile(*ncid); return rc; }
  }
  if (has_unlim_dim) {
    int unlimdim = -1, ndims = 0, dimids[8] = {0};
    PIOc_inq_unlimdim(*ncid, &unlimdim);
    PIOc_inq_varndims(*ncid, *varid, &ndims);
    PIOc_inq_vardimid(*ncid, *varid, dimids);
    *has_unlim_dim = false;
    for (int k = 0; k < ndims; ++k)
      if (dimids[k] == unlimdim && unlimdim >= 0) *has_unlim_dim = true;
  }
  return PIO_NOERR;
}

int readDecomposed(const std::string& path, const std::string& varname,
                   int domain_handle, int stagger, int timelevel, int nz,
                   int nz2, double* buf) {
  int ncid, varid;
  bool has_t = false;
  int rc = openAndFind(path, varname, &ncid, &varid, &has_t);
  if (rc != PIO_NOERR) return rc;
  if (debugOn() && amRoot())
    std::fprintf(stderr, "TIM_IO read_dd  %s:%s stag=%d nz=%d nz2=%d tl=%d\n",
                 path.c_str(), varname.c_str(), stagger, nz, nz2, timelevel);

  const DomainInfo& d = domains[(size_t)domain_handle];
  int gnx, gny, ws, we, wjs, wje;
  staggeredExtents(d, stagger, gnx, gny, ws, we, wjs, wje);
  const int wni = we - ws + 1, wnj = wje - wjs + 1;

  if (has_t) PIOc_setframe(ncid, varid, timelevel > 0 ? timelevel - 1 : 0);
  static double dummyd = 0.0;

  // Read each disjoint component and scatter it into the full window buffer
  // (x fastest, then y, then k), so the caller sees one contiguous window.
  std::vector<double> piece;
  for (int comp = 0; comp < 4; ++comp) {
    int is, ie, js, je;
    if (!componentWindow(d, stagger, comp, is, ie, js, je)) continue;
    const int ni = ie - is + 1, nj = je - js + 1;
    const PIO_Offset maplen = (PIO_Offset)ni * nj * nz;
    int ioid = getDecomp(domain_handle, stagger, nz, nz2, comp);
    piece.assign((size_t)maplen, 0.0);
    rc = PIOc_read_darray(ncid, varid, ioid, maplen,
                          maplen ? piece.data() : &dummyd);
    if (rc != PIO_NOERR) {
      std::fprintf(stderr, "tim_io: read_darray(%s:%s comp %d) rc=%d\n",
                   path.c_str(), varname.c_str(), comp, rc);
      PIOc_closefile(ncid);
      return rc;
    }
    for (int k = 0; k < nz; ++k)
      for (int j = js; j <= je; ++j)
        for (int i = is; i <= ie; ++i)
          buf[(size_t)k * wni * wnj + (size_t)(j - wjs) * wni + (i - ws)] =
              piece[(size_t)k * ni * nj + (size_t)(j - js) * ni + (i - is)];
  }
  PIOc_closefile(ncid);
  return PIO_NOERR;
}

int readPlain(const std::string& path, const std::string& varname,
              int timelevel, int n, double* buf) {
  if (debugOn() && amRoot())
    std::fprintf(stderr, "TIM_IO read_pl  %s:%s n=%d tl=%d\n", path.c_str(),
                 varname.c_str(), n, timelevel);
  int ncid, varid;
  bool has_t = false;
  int rc = openAndFind(path, varname, &ncid, &varid, &has_t);
  if (rc != PIO_NOERR) return rc;

  int ndims = 0;
  PIOc_inq_varndims(ncid, varid, &ndims);
  PIO_Offset start[4] = {0, 0, 0, 0}, count[4] = {1, 1, 1, 1};
  int k = 0;
  if (has_t && ndims > 0) { start[0] = timelevel > 0 ? timelevel - 1 : 0; k = 1; }
  if (ndims > k) count[ndims - 1] = n;  // last dim is the data extent
  rc = PIOc_get_vara_double(ncid, varid, start, count, buf);
  PIOc_closefile(ncid);
  return rc;
}

bool findVar(const std::string& path, const std::string& varname,
             std::string& actual_name) {
  int ncid, varid;
  int rc = openAndFind(path, varname, &ncid, &varid, nullptr);
  if (rc != PIO_NOERR) return false;
  char name[PIO_MAX_NAME + 1] = {0};
  PIOc_inq_varname(ncid, varid, name);
  actual_name = name;
  PIOc_closefile(ncid);
  return true;
}

void finalize() {
  if (iosysid >= 0) { PIOc_finalize(iosysid); iosysid = -1; }
  decomp_cache.clear();
  domains.clear();
}

}  // namespace IO
}  // namespace TIM

// ---------------------------------------------------------------- C API ----

extern "C" {

int tim_io_register_domain(int nig, int njg, int isc, int iec, int jsc,
                                int jec, int symmetric) {
  TIM::IO::DomainInfo d;
  d.nig = nig; d.njg = njg;
  d.isc = isc; d.iec = iec; d.jsc = jsc; d.jec = jec;
  d.symmetric = symmetric;
  return TIM::IO::registerDomain(d);
}

int tim_io_read_decomposed(const char* path, const char* varname,
                                int domain_handle, int stagger, int timelevel,
                                int nz, int nz2, double* buf) {
  return TIM::IO::readDecomposed(path, varname, domain_handle, stagger,
                                 timelevel, nz, nz2, buf);
}

int tim_io_read_plain(const char* path, const char* varname,
                           int timelevel, int n, double* buf) {
  return TIM::IO::readPlain(path, varname, timelevel, n, buf);
}

int tim_io_var_exists(const char* path, const char* varname) {
  std::string actual;
  return TIM::IO::findVar(path, varname, actual) ? 1 : 0;
}

void tim_io_finalize() { TIM::IO::finalize(); }

}  // extern "C"
