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

int iosysId() { ensureInit(); return iosysid; }

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
                   int nz2, double* buf, int* file_sx, int* file_sy) {
  int ncid, varid;
  bool has_t = false;
  int rc = openAndFind(path, varname, &ncid, &varid, &has_t);
  if (rc != PIO_NOERR) return rc;

  const DomainInfo& d = domains[(size_t)domain_handle];

  // The FILE determines the staggered axis sizes, not the model domain:
  // symmetric-memory cases may write staggered axes of size nig (dropping the
  // low edge) or nig+1 depending on config (FMS size-sniffs; so do we).
  // Inspect the variable's x (last) and y (second-last) dim lengths.
  int ndims = 0, dimids[8] = {0};
  PIOc_inq_varndims(ncid, varid, &ndims);
  PIOc_inq_vardimid(ncid, varid, dimids);
  PIO_Offset xlen = 0, ylen = 0;
  if (ndims >= 1) PIOc_inq_dimlen(ncid, dimids[ndims - 1], &xlen);
  if (ndims >= 2) PIOc_inq_dimlen(ncid, dimids[ndims - 2], &ylen);
  const bool want_sx = (stagger == EAST_FACE || stagger == CORNER) && d.symmetric;
  const bool want_sy = (stagger == NORTH_FACE || stagger == CORNER) && d.symmetric;
  const int sxf = (want_sx && xlen == (PIO_Offset)d.nig + 1) ? 1 : 0;
  const int syf = (want_sy && ylen == (PIO_Offset)d.njg + 1) ? 1 : 0;
  if (file_sx) *file_sx = sxf;
  if (file_sy) *file_sy = syf;
  // Effective stagger for decomp construction = what the file actually has.
  int fstag = CENTER;
  if (sxf && syf) fstag = CORNER;
  else if (sxf) fstag = EAST_FACE;
  else if (syf) fstag = NORTH_FACE;

  if (debugOn() && amRoot())
    std::fprintf(stderr,
                 "TIM_IO read_dd  %s:%s stag=%d(file %d) nz=%d nz2=%d tl=%d\n",
                 path.c_str(), varname.c_str(), stagger, fstag, nz, nz2,
                 timelevel);

  // Window buffer layout is fixed by the CALLER's (domain) staggering; data
  // from a less-staggered file lands shifted by (want - file) per dim, and the
  // low-edge column/row that the file lacks is left untouched (the model fills
  // it by halo/edge updates afterward, matching FMS behavior).
  int gnx, gny, ws, we, wjs, wje;
  staggeredExtents(d, stagger, gnx, gny, ws, we, wjs, wje);
  const int wni = we - ws + 1, wnj = wje - wjs + 1;
  const int shx = (want_sx ? 1 : 0) - sxf, shy = (want_sy ? 1 : 0) - syf;

  if (has_t) PIOc_setframe(ncid, varid, timelevel > 0 ? timelevel - 1 : 0);
  static double dummyd = 0.0;

  // Read each disjoint component and scatter it into the window buffer
  // (x fastest, then y, then k), so the caller sees one contiguous window.
  std::vector<double> piece;
  for (int comp = 0; comp < 4; ++comp) {
    int is, ie, js, je;
    if (!componentWindow(d, fstag, comp, is, ie, js, je)) continue;
    const int ni = ie - is + 1, nj = je - js + 1;
    const PIO_Offset maplen = (PIO_Offset)ni * nj * nz;
    int ioid = getDecomp(domain_handle, fstag, nz, nz2, comp);
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
          buf[(size_t)k * wni * wnj + (size_t)(j - wjs + shy) * wni +
              (i - ws + shx)] =
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

// ------------------------------------------------------------ write path ---
// Stateful write files: MOM registers axes (dims + coordinate vars + attrs),
// then variables (dims by axis name), then writes axis values and field data.
// The variable's staggering and z-extent are implicit in its registered dims,
// so the file registry remembers each axis's kind.

struct WAxis {
  int kind = 3;      // 0=x, 1=y, 2=unlimited(time), 3=fixed length
  int position = 0;  // Stagger for x/y axes (CENTER/EAST_FACE/NORTH_FACE)
  int len = 0;       // global length for fixed axes
};
struct WVar {
  int stagger = CENTER;
  int nz = 1, nz2 = 1;   // trailing extents (product of fixed dims; nz2 = 4th)
  bool has_time = false;
};
struct WFile {
  int ncid = -1;
  int domain_handle = -1;
  bool in_def = true;
  int num_times = 0;
  double file_time = 0.0;
  std::string unlim_name;
  std::map<std::string, WAxis> axes;
  std::map<std::string, WVar> vars;
};
static std::vector<WFile> wfiles;

// Write partition: every global point exactly once. Staggered (+1) axes give
// the extra east/north point to the east/north-most rank only (unlike the
// overlapping READ windows). kind flag separates these decomps in the cache.
static int getWriteDecomp(int domain_handle, int stagger, int nz, int nz2) {
  const long long key = (1LL << 60) | ((long long)domain_handle << 44) |
                        ((long long)stagger << 40) | ((long long)nz2 << 20) | nz;
  auto it = decomp_cache.find(key);
  if (it != decomp_cache.end()) return it->second;

  const DomainInfo& d = domains[(size_t)domain_handle];
  const bool sx = (stagger == EAST_FACE || stagger == CORNER) && d.symmetric;
  const bool sy = (stagger == NORTH_FACE || stagger == CORNER) && d.symmetric;
  const int gnx = d.nig + (sx ? 1 : 0), gny = d.njg + (sy ? 1 : 0);
  const int is = d.isc, ie = d.iec + ((sx && d.iec == d.nig) ? 1 : 0);
  const int js = d.jsc, je = d.jec + ((sy && d.jec == d.njg) ? 1 : 0);

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
  static PIO_Offset dummy = 0;
  int ioid, rearr = PIO_REARR_BOX;
  int rc = PIOc_InitDecomp(iosysid, PIO_DOUBLE, ndims, gdims, (int)dof.size(),
                           dof.empty() ? &dummy : dof.data(), &ioid, &rearr,
                           nullptr, nullptr);
  if (rc != PIO_NOERR) {
    std::fprintf(stderr, "tim_io: write InitDecomp failed rc=%d\n", rc);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  decomp_cache[key] = ioid;
  return ioid;
}

static void endDef(WFile& w) {
  if (w.in_def) { PIOc_enddef(w.ncid); w.in_def = false; }
}

int createFile(const std::string& path, int domain_handle, int mode) {
  ensureInit();
  WFile w;
  w.domain_handle = domain_handle;
  int iotype = PIO_IOTYPE_PNETCDF;
  int rc;
  if (mode == 2) {  // append: recover record state, reenter define mode
    rc = PIOc_openfile(iosysid, &w.ncid, &iotype, path.c_str(), PIO_WRITE);
    if (rc == PIO_NOERR) {
      int unlimdim = -1;
      PIOc_inq_unlimdim(w.ncid, &unlimdim);
      if (unlimdim >= 0) {
        char uname[PIO_MAX_NAME + 1] = {0};
        PIO_Offset ulen = 0;
        PIOc_inq_dim(w.ncid, unlimdim, uname, &ulen);
        w.unlim_name = uname;
        w.num_times = (int)ulen;
        if (ulen > 0) {
          int uvar = -1;
          if (PIOc_inq_varid(w.ncid, uname, &uvar) == PIO_NOERR) {
            PIO_Offset s = ulen - 1, c = 1;
            PIOc_get_vara_double(w.ncid, uvar, &s, &c, &w.file_time);
          }
        }
      }
      w.in_def = false;
    }
  } else {  // write / overwrite: clobber (FMS "write" on new files behaves so)
    rc = PIOc_createfile(iosysid, &w.ncid, &iotype, path.c_str(),
                         PIO_CLOBBER | PIO_64BIT_OFFSET);  // match FMS format
  }
  if (rc != PIO_NOERR) {
    std::fprintf(stderr, "tim_io: create/open %s (mode %d) rc=%d\n",
                 path.c_str(), mode, rc);
    return -1;
  }
  wfiles.push_back(w);
  return (int)wfiles.size() - 1;
}

// Define a dim + its coordinate variable with attributes. kind/position per
// WAxis; n is the global length for fixed axes (z etc.) and for decomposed
// axes (which get gnx/gny+shift from the domain).
int defAxis(int fh, const std::string& name, int kind, int position, int n,
            const std::string& units, const std::string& longname,
            const std::string& cartesian, int sense, int has_sense) {
  WFile& w = wfiles[(size_t)fh];
  const DomainInfo& d = domains[(size_t)w.domain_handle];
  int dimid, len = n;
  if (kind == 0) len = d.nig + ((position == EAST_FACE || position == CORNER) &&
                                d.symmetric ? 1 : 0);
  if (kind == 1) len = d.njg + ((position == NORTH_FACE || position == CORNER) &&
                                d.symmetric ? 1 : 0);
  int rc = PIOc_def_dim(w.ncid, name.c_str(),
                        kind == 2 ? PIO_UNLIMITED : (PIO_Offset)len, &dimid);
  if (rc != PIO_NOERR) return rc;
  int varid;
  rc = PIOc_def_var(w.ncid, name.c_str(), PIO_DOUBLE, 1, &dimid, &varid);
  if (rc != PIO_NOERR) return rc;
  if (!longname.empty())
    PIOc_put_att_text(w.ncid, varid, "long_name", longname.size(), longname.c_str());
  if (!units.empty())
    PIOc_put_att_text(w.ncid, varid, "units", units.size(), units.c_str());
  if (!cartesian.empty())
    PIOc_put_att_text(w.ncid, varid, "cartesian_axis", cartesian.size(),
                      cartesian.c_str());
  if (has_sense)
    PIOc_put_att_int(w.ncid, varid, "sense", PIO_INT, 1, &sense);
  WAxis a; a.kind = kind; a.position = position; a.len = len;
  w.axes[name] = a;
  if (kind == 2) w.unlim_name = name;
  return PIO_NOERR;
}

// Register a variable; dims given as a '\n'-joined list of axis names,
// x/y/time kinds inferred from the axis registry.
int defVar(int fh, const std::string& name, const std::string& dims_joined,
           const std::string& units, const std::string& longname,
           const std::string& std_name, int pack, const std::string& checksum) {
  WFile& w = wfiles[(size_t)fh];
  std::vector<std::string> dims;
  size_t p = 0;
  while (p < dims_joined.size()) {
    size_t q = dims_joined.find('\n', p);
    if (q == std::string::npos) q = dims_joined.size();
    if (q > p) dims.push_back(dims_joined.substr(p, q - p));
    p = q + 1;
  }
  WVar v;
  std::vector<int> dimids;
  int sx = 0, sy = 0;
  std::vector<int> zlens;
  for (const auto& dn : dims) {
    int dimid;
    if (PIOc_inq_dimid(w.ncid, dn.c_str(), &dimid) != PIO_NOERR) return -1;
    dimids.push_back(dimid);
    const WAxis& a = w.axes[dn];
    if (a.kind == 0 && (a.position == EAST_FACE || a.position == CORNER)) sx = 1;
    if (a.kind == 1 && (a.position == NORTH_FACE || a.position == CORNER)) sy = 1;
    if (a.kind == 2) v.has_time = true;
    if (a.kind == 3) zlens.push_back(a.len);
  }
  v.stagger = sx && sy ? CORNER : sx ? EAST_FACE : sy ? NORTH_FACE : CENTER;
  v.nz = 1;
  for (int zl : zlens) v.nz *= zl;
  v.nz2 = (zlens.size() > 1) ? zlens.back() : 1;
  // netCDF wants dims slowest-first; MOM passes axes in Fortran order (x first)
  std::vector<int> cdims(dimids.rbegin(), dimids.rend());
  int varid;
  int rc = PIOc_def_var(w.ncid, name.c_str(), pack > 1 ? PIO_FLOAT : PIO_DOUBLE,
                        (int)cdims.size(), cdims.data(), &varid);
  if (rc != PIO_NOERR) return rc;
  if (!longname.empty())
    PIOc_put_att_text(w.ncid, varid, "long_name", longname.size(), longname.c_str());
  if (!units.empty())
    PIOc_put_att_text(w.ncid, varid, "units", units.size(), units.c_str());
  if (!std_name.empty())
    PIOc_put_att_text(w.ncid, varid, "standard_name", std_name.size(),
                      std_name.c_str());
  if (!checksum.empty())
    PIOc_put_att_text(w.ncid, varid, "checksum", checksum.size(), checksum.c_str());
  w.vars[name] = v;
  return PIO_NOERR;
}

int putGlobalAtt(int fh, const std::string& name, const std::string& value) {
  WFile& w = wfiles[(size_t)fh];
  return PIOc_put_att_text(w.ncid, PIO_GLOBAL, name.c_str(), value.size(),
                           value.c_str());
}

// Write coordinate values (global; identical on all ranks; collective).
int writeAxis(int fh, const std::string& name, const double* data, int n) {
  WFile& w = wfiles[(size_t)fh];
  endDef(w);
  int varid;
  int rc = PIOc_inq_varid(w.ncid, name.c_str(), &varid);
  if (rc != PIO_NOERR) return rc;
  PIO_Offset s = 0, c = n;
  return PIOc_put_vara_double(w.ncid, varid, &s, &c, data);
}

// FMS write_time_if_later semantics: advance the record when t is newer.
static int frameForTime(WFile& w, double t, bool has_tstamp) {
  if (!has_tstamp) return -1;
  if (t > w.file_time || w.num_times == 0) {
    w.file_time = t;
    w.num_times += 1;
    if (!w.unlim_name.empty()) {
      int uvar;
      if (PIOc_inq_varid(w.ncid, w.unlim_name.c_str(), &uvar) == PIO_NOERR) {
        PIO_Offset s = w.num_times - 1, c = 1;
        PIOc_put_vara_double(w.ncid, uvar, &s, &c, &t);
      }
    }
  }
  return w.num_times - 1;
}

// stagger for a registered variable (Fortran needs it to size the window).
int varStagger(int fh, const std::string& name) {
  WFile& w = wfiles[(size_t)fh];
  auto it = w.vars.find(name);
  return it == w.vars.end() ? -1 : it->second.stagger;
}

// Decomposed write from the caller's staggered window buffer (same layout as
// the read window: [isc..iec+sx] x [jsc..jec+sy], x fastest). The partition
// subset is gathered out of the window.
int writeDecomposed(int fh, const std::string& name, const double* buf,
                    double tstamp, int has_tstamp) {
  WFile& w = wfiles[(size_t)fh];
  auto it = w.vars.find(name);
  if (it == w.vars.end()) return -1;
  const WVar& v = it->second;
  endDef(w);
  int varid;
  int rc = PIOc_inq_varid(w.ncid, name.c_str(), &varid);
  if (rc != PIO_NOERR) return rc;
  const int frame = frameForTime(w, tstamp, has_tstamp != 0);
  if (v.has_time && frame >= 0) PIOc_setframe(w.ncid, varid, frame);

  const DomainInfo& d = domains[(size_t)w.domain_handle];
  const bool sx = (v.stagger == EAST_FACE || v.stagger == CORNER) && d.symmetric;
  const bool sy = (v.stagger == NORTH_FACE || v.stagger == CORNER) && d.symmetric;
  const int wni = d.iec - d.isc + 1 + (sx ? 1 : 0);
  const int wnj = d.jec - d.jsc + 1 + (sy ? 1 : 0);
  const int ie = d.iec + ((sx && d.iec == d.nig) ? 1 : 0);
  const int je = d.jec + ((sy && d.jec == d.njg) ? 1 : 0);
  const int ni = ie - d.isc + 1, nj = je - d.jsc + 1;

  std::vector<double> part((size_t)ni * nj * v.nz);
  for (int k = 0; k < v.nz; ++k)
    for (int j = 0; j < nj; ++j)
      for (int i = 0; i < ni; ++i)
        part[(size_t)k * ni * nj + (size_t)j * ni + i] =
            buf[(size_t)k * wni * wnj + (size_t)j * wni + i];

  int ioid = getWriteDecomp(w.domain_handle, v.stagger, v.nz, v.nz2);
  static double dummyd = 0.0;
  rc = PIOc_write_darray(w.ncid, varid, ioid, (PIO_Offset)part.size(),
                         part.empty() ? &dummyd : part.data(), nullptr);
  if (rc != PIO_NOERR)
    std::fprintf(stderr, "tim_io: write_darray(%s) rc=%d\n", name.c_str(), rc);
  if (debugOn() && amRoot())
    std::fprintf(stderr, "TIM_IO write_dd %s stag=%d nz=%d frame=%d rc=%d\n",
                 name.c_str(), v.stagger, v.nz, frame, rc);
  return rc;
}

// Non-decomposed (0d/1d) write; values identical on all ranks; collective.
int writePlain(int fh, const std::string& name, const double* data, int n,
               double tstamp, int has_tstamp) {
  WFile& w = wfiles[(size_t)fh];
  endDef(w);
  int varid;
  int rc = PIOc_inq_varid(w.ncid, name.c_str(), &varid);
  if (rc != PIO_NOERR) return rc;
  const int frame = frameForTime(w, tstamp, has_tstamp != 0);
  const bool has_t = w.vars.count(name) ? w.vars[name].has_time : false;
  PIO_Offset start[2] = {0, 0}, count[2] = {1, 1};
  int nd = 1;
  if (has_t && frame >= 0) { start[0] = frame; count[1] = n; nd = 2; }
  else count[0] = n;
  (void)nd;
  if (debugOn() && amRoot())
    std::fprintf(stderr, "TIM_IO write_pl %s n=%d frame=%d\n", name.c_str(), n,
                 frame);
  return PIOc_put_vara_double(w.ncid, varid, start, count, data);
}

int closeFile(int fh) {
  WFile& w = wfiles[(size_t)fh];
  if (w.ncid < 0) return 0;
  endDef(w);
  int rc = PIOc_closefile(w.ncid);
  w.ncid = -1;
  return rc;
}

int fileNumTimes(int fh) { return wfiles[(size_t)fh].num_times; }
double fileTime(int fh) { return wfiles[(size_t)fh].file_time; }

void finalize() {
  if (iosysid >= 0) { PIOc_finalize(iosysid); iosysid = -1; }
  decomp_cache.clear();
  domains.clear();
  wfiles.clear();
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
                                int nz, int nz2, double* buf, int* file_sx,
                                int* file_sy) {
  return TIM::IO::readDecomposed(path, varname, domain_handle, stagger,
                                 timelevel, nz, nz2, buf, file_sx, file_sy);
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

/* ---- write path ---- */

int tim_io_createfile(const char* path, int domain_handle, int mode) {
  return TIM::IO::createFile(path, domain_handle, mode);
}

int tim_io_def_axis(int fh, const char* name, int kind, int position, int n,
                    const char* units, const char* longname,
                    const char* cartesian, int sense, int has_sense) {
  return TIM::IO::defAxis(fh, name, kind, position, n, units, longname,
                          cartesian, sense, has_sense);
}

int tim_io_def_var(int fh, const char* name, const char* dims_joined,
                   const char* units, const char* longname,
                   const char* std_name, int pack, const char* checksum) {
  return TIM::IO::defVar(fh, name, dims_joined, units, longname, std_name,
                         pack, checksum);
}

int tim_io_put_global_att(int fh, const char* name, const char* value) {
  return TIM::IO::putGlobalAtt(fh, name, value);
}

int tim_io_write_axis(int fh, const char* name, const double* data, int n) {
  return TIM::IO::writeAxis(fh, name, data, n);
}

int tim_io_var_stagger(int fh, const char* name) {
  return TIM::IO::varStagger(fh, name);
}

int tim_io_write_decomposed(int fh, const char* name, const double* buf,
                            double tstamp, int has_tstamp) {
  return TIM::IO::writeDecomposed(fh, name, buf, tstamp, has_tstamp);
}

int tim_io_write_plain(int fh, const char* name, const double* data, int n,
                       double tstamp, int has_tstamp) {
  return TIM::IO::writePlain(fh, name, data, n, tstamp, has_tstamp);
}

int tim_io_closefile(int fh) { return TIM::IO::closeFile(fh); }

int tim_io_file_num_times(int fh) { return TIM::IO::fileNumTimes(fh); }
double tim_io_file_time(int fh) { return TIM::IO::fileTime(fh); }

}  // extern "C"
