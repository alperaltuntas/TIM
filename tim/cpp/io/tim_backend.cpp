// TIM::IO backend seam over ParallelIO — the only TU that includes pio.h.
// Restricted to the PIOc_* subset PIO2 and SCORPIO share.

#include "tim_backend.hpp"

#include <mpi.h>
#include <netcdf.h>
#include <pio.h>

#include <cctype>
#include <cstdio>

namespace TIM {
namespace IO {

SysId Backend::init(MPI_Comm comm, int niotasks, int stride) {
  int iosysid = -1;
  int rc = PIOc_Init_Intracomm(comm, niotasks, stride, 0,
                               PIO_REARR_BOX, &iosysid);
  if (rc != PIO_NOERR) {
    std::fprintf(stderr, "TIM backend: Init_Intracomm rc=%d\n", rc);
    MPI_Abort(comm, 1);
  }
  PIOc_set_iosystem_error_handling(iosysid, PIO_RETURN_ERROR, nullptr);
  return iosysid;
}

void Backend::finalize(SysId sys) { PIOc_finalize(sys); }

DecompId Backend::initDecomp(SysId sys, int ndims, const int* gdims,
                             const std::vector<long long>& dof, bool single) {
  static PIO_Offset dummy = 0;  // PIO rejects NULL maps even when empty
  std::vector<PIO_Offset> map(dof.begin(), dof.end());
  int ioid = -1, rearr = PIO_REARR_BOX;
  int rc = PIOc_InitDecomp(sys, single ? PIO_REAL : PIO_DOUBLE, ndims, gdims,
                           (int)map.size(), map.empty() ? &dummy : map.data(),
                           &ioid, &rearr, nullptr, nullptr);
  if (rc != PIO_NOERR) {
    std::fprintf(stderr, "TIM backend: InitDecomp rc=%d\n", rc);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  return ioid;
}

void Backend::freeDecomp(SysId sys, DecompId id) { PIOc_freedecomp(sys, id); }

FileId Backend::openRead(SysId sys, const std::string& path) {
  int ncid, iotype = PIO_IOTYPE_PNETCDF;
  int rc = PIOc_openfile(sys, &ncid, &iotype, path.c_str(), PIO_NOWRITE);
  if (rc != PIO_NOERR) {  // HDF5-based files need the other iotype
    iotype = PIO_IOTYPE_NETCDF4P;
    rc = PIOc_openfile(sys, &ncid, &iotype, path.c_str(), PIO_NOWRITE);
  }
  return rc == PIO_NOERR ? ncid : -1;
}

FileId Backend::openWrite(SysId sys, const std::string& path) {
  int ncid, iotype = PIO_IOTYPE_PNETCDF;
  int rc = PIOc_openfile(sys, &ncid, &iotype, path.c_str(), PIO_WRITE);
  return rc == PIO_NOERR ? ncid : -1;
}

FileId Backend::create(SysId sys, const std::string& path) {
  int ncid, iotype = PIO_IOTYPE_PNETCDF;
  int rc = PIOc_createfile(sys, &ncid, &iotype, path.c_str(),
                           PIO_CLOBBER | PIO_64BIT_OFFSET);
  return rc == PIO_NOERR ? ncid : -1;
}

int Backend::close(FileId f) { return PIOc_closefile(f); }
int Backend::endDef(FileId f) { return PIOc_enddef(f); }

int Backend::defDim(FileId f, const std::string& name, long long len,
                    bool unlimited, int* dimid) {
  return PIOc_def_dim(f, name.c_str(),
                      unlimited ? PIO_UNLIMITED : (PIO_Offset)len, dimid);
}
int Backend::inqDimId(FileId f, const std::string& name, int* dimid) {
  return PIOc_inq_dimid(f, name.c_str(), dimid);
}
int Backend::defVar(FileId f, const std::string& name, bool single_precision,
                    const std::vector<int>& dimids, VarId* varid) {
  return PIOc_def_var(f, name.c_str(),
                      single_precision ? PIO_FLOAT : PIO_DOUBLE,
                      (int)dimids.size(), dimids.data(), varid);
}
int Backend::putAttText(FileId f, VarId v, const std::string& name,
                        const std::string& value) {
  return PIOc_put_att_text(f, v, name.c_str(), value.size(), value.c_str());
}
int Backend::putAttInt(FileId f, VarId v, const std::string& name, int value) {
  return PIOc_put_att_int(f, v, name.c_str(), PIO_INT, 1, &value);
}
int Backend::putAttInts(FileId f, VarId v, const std::string& name,
                        const int* values, int n) {
  return PIOc_put_att_int(f, v, name.c_str(), PIO_INT, n, values);
}
int Backend::putAttDouble(FileId f, VarId v, const std::string& name,
                          const double* values, int n, bool as_float) {
  if (as_float) {
    std::vector<float> fv(values, values + n);
    return PIOc_put_att_float(f, v, name.c_str(), PIO_FLOAT, n, fv.data());
  }
  return PIOc_put_att_double(f, v, name.c_str(), PIO_DOUBLE, n, values);
}
int Backend::defVarFillValue(FileId f, VarId v, bool single_precision,
                             double value) {
  if (single_precision) {
    float fv = (float)value;
    return PIOc_def_var_fill(f, v, 0 /*fill mode on*/, &fv);
  }
  return PIOc_def_var_fill(f, v, 0 /*fill mode on*/, &value);
}
int Backend::defVarFill(FileId f, VarId v, bool single_precision) {
  static double dfill = 9.9692099683868690e+36;  // NC_FILL_DOUBLE
  static float ffill = 9.9692099683868690e+36f;  // NC_FILL_FLOAT
  return PIOc_def_var_fill(f, v, 0 /*fill mode on*/,
                           single_precision ? (void*)&ffill : (void*)&dfill);
}

VarId Backend::globalAtts() { return PIO_GLOBAL; }

int Backend::findVar(FileId f, const std::string& name, VarId* v) {
  return PIOc_inq_varid(f, name.c_str(), v);
}
int Backend::varName(FileId f, VarId v, std::string* name) {
  char buf[PIO_MAX_NAME + 1] = {0};
  int rc = PIOc_inq_varname(f, v, buf);
  if (rc == PIO_NOERR) *name = buf;
  return rc;
}
int Backend::numVars(FileId f, int* n) { return PIOc_inq_nvars(f, n); }
int Backend::varNumDims(FileId f, VarId v, int* n) {
  return PIOc_inq_varndims(f, v, n);
}
int Backend::varDimIds(FileId f, VarId v, int* dimids) {
  return PIOc_inq_vardimid(f, v, dimids);
}
int Backend::dimLen(FileId f, int dimid, long long* len) {
  PIO_Offset l = 0;
  int rc = PIOc_inq_dimlen(f, dimid, &l);
  *len = l;
  return rc;
}
int Backend::unlimDim(FileId f, int* dimid) { return PIOc_inq_unlimdim(f, dimid); }
int Backend::numDims(FileId f, int* n) { return PIOc_inq_ndims(f, n); }
int Backend::getAttText(FileId f, VarId v, const std::string& name,
                        std::string* out) {
  PIO_Offset len = 0;
  int rc = PIOc_inq_attlen(f, v, name.c_str(), &len);
  if (rc != PIO_NOERR) return rc;
  std::vector<char> buf((size_t)len + 1, '\0');
  rc = PIOc_get_att_text(f, v, name.c_str(), buf.data());
  if (rc == PIO_NOERR) *out = std::string(buf.data(), (size_t)len);
  return rc;
}

int Backend::setFrame(FileId f, VarId v, int frame) {
  return PIOc_setframe(f, v, frame);
}
int Backend::readDArray(FileId f, VarId v, DecompId d, long long n, double* buf,
                        bool single) {
  if (single) {
    static float fdummy = 0.0f;
    std::vector<float> fbuf((size_t)n);
    int rc = PIOc_read_darray(f, v, d, (PIO_Offset)n,
                              n ? (void*)fbuf.data() : (void*)&fdummy);
    for (long long i = 0; i < n; ++i) buf[i] = (double)fbuf[(size_t)i];
    return rc;
  }
  static double dummy = 0.0;
  return PIOc_read_darray(f, v, d, (PIO_Offset)n, n ? buf : &dummy);
}
int Backend::inqVarSingle(FileId f, VarId v, bool* is_single) {
  nc_type t = NC_NAT;
  int rc = PIOc_inq_vartype(f, v, &t);
  if (rc == PIO_NOERR) *is_single = (t == PIO_FLOAT);
  return rc;
}
int Backend::writeDArray(FileId f, VarId v, DecompId d, long long n,
                         const double* buf, double fill, bool single) {
  // Cells covered by no rank (masked/eliminated tiles) get the variable's own
  // fill value — PIO rejects a fill argument that differs from the var's.
  if (single) {
    static float fdummy = 0.0f;
    std::vector<float> fbuf(buf, buf + n);
    float ffill = (float)fill;
    return PIOc_write_darray(f, v, d, (PIO_Offset)n,
                             n ? (void*)fbuf.data() : (void*)&fdummy, &ffill);
  }
  static double dummy = 0.0;
  return PIOc_write_darray(f, v, d, (PIO_Offset)n,
                           const_cast<double*>(n ? buf : &dummy), &fill);
}
int Backend::getVaraDouble(FileId f, VarId v, const long long start[],
                           const long long count[], int ndims, double* buf) {
  PIO_Offset s[8], c[8];
  for (int i = 0; i < ndims; ++i) { s[i] = start[i]; c[i] = count[i]; }
  return PIOc_get_vara_double(f, v, s, c, buf);
}
int Backend::putVaraDouble(FileId f, VarId v, const long long start[],
                           const long long count[], int ndims,
                           const double* buf) {
  PIO_Offset s[8], c[8];
  for (int i = 0; i < ndims; ++i) { s[i] = start[i]; c[i] = count[i]; }
  return PIOc_put_vara_double(f, v, s, c, buf);
}

// ---- Backend::Serial: rank-independent plain-netCDF access ----

namespace {
bool ciEq(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
      return false;
  return *a == *b;
}
int sFindVarCI(int nc, const std::string& var, int* v, std::string* actual) {
  if (nc_inq_varid(nc, var.c_str(), v) == NC_NOERR) {
    if (actual) *actual = var;
    return 0;
  }
  int nvars = 0;
  nc_inq_nvars(nc, &nvars);
  char nm[NC_MAX_NAME + 1];
  for (int cand = 0; cand < nvars; ++cand) {
    nc_inq_varname(nc, cand, nm);
    if (ciEq(nm, var.c_str())) {
      *v = cand;
      if (actual) *actual = nm;
      return 0;
    }
  }
  return -1;
}
}  // namespace

bool Backend::Serial::fileExists(const std::string& path) {
  int nc;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return false;
  nc_close(nc);
  return true;
}

int Backend::Serial::findVarCI(const std::string& path, const std::string& var,
                               std::string* actual) {
  int nc, v;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  int rc = sFindVarCI(nc, var, &v, actual);
  nc_close(nc);
  return rc;
}

int Backend::Serial::fileInfo(const std::string& path, int* ndims, int* nvars,
                              int* ntimes) {
  int nc;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  int nd = 0, nv = 0, unlim = -1;
  size_t ul = 0;
  nc_inq(nc, &nd, &nv, nullptr, &unlim);
  if (unlim >= 0) nc_inq_dimlen(nc, unlim, &ul);
  nc_close(nc);
  if (ndims) *ndims = nd;
  if (nvars) *nvars = nv;
  if (ntimes) *ntimes = (int)ul;
  return 0;
}

int Backend::Serial::timeValues(const std::string& path, double* buf, int n) {
  int nc, unlim = -1;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  nc_inq_unlimdim(nc, &unlim);
  int rc = -1;
  if (unlim >= 0) {
    int nvars = 0;
    nc_inq_nvars(nc, &nvars);
    for (int v = 0; v < nvars; ++v) {
      int nd = 0, dimids[NC_MAX_VAR_DIMS];
      nc_inq_varndims(nc, v, &nd);
      nc_inq_vardimid(nc, v, dimids);
      if (nd == 1 && dimids[0] == unlim) {
        size_t s = 0, c = (size_t)n;
        rc = (nc_get_vara_double(nc, v, &s, &c, buf) == NC_NOERR) ? 0 : -1;
        break;
      }
    }
  }
  nc_close(nc);
  return rc;
}

int Backend::Serial::varNameAt(const std::string& path, int index0,
                               std::string* name) {
  int nc;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  char nm[NC_MAX_NAME + 1] = {0};
  int rc = (nc_inq_varname(nc, index0, nm) == NC_NOERR) ? 0 : -1;
  nc_close(nc);
  if (rc == 0) *name = nm;
  return rc;
}

int Backend::Serial::attText(const std::string& path, const std::string& var,
                             const std::string& att, std::string* out) {
  int nc, v;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  int rc = sFindVarCI(nc, var, &v, nullptr);
  if (rc == 0) {
    size_t len = 0;
    rc = (nc_inq_attlen(nc, v, att.c_str(), &len) == NC_NOERR) ? 0 : -1;
    if (rc == 0) {
      std::vector<char> b(len + 1, '\0');
      rc = (nc_get_att_text(nc, v, att.c_str(), b.data()) == NC_NOERR) ? 0 : -1;
      if (rc == 0) *out = std::string(b.data(), len);
    }
  }
  nc_close(nc);
  return rc;
}

int Backend::Serial::attDouble(const std::string& path, const std::string& var,
                               const std::string& att, double* out, int n) {
  int nc, v;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  int rc = sFindVarCI(nc, var, &v, nullptr);
  if (rc == 0) {
    size_t len = 0;
    rc = (nc_inq_attlen(nc, v, att.c_str(), &len) == NC_NOERR &&
          (int)len >= n) ? 0 : -1;
    if (rc == 0)
      rc = (nc_get_att_double(nc, v, att.c_str(), out) == NC_NOERR) ? 0 : -1;
  }
  nc_close(nc);
  return rc;
}

int Backend::Serial::timeName(const std::string& path, std::string* name) {
  int nc, unlim = -1;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  nc_inq_unlimdim(nc, &unlim);
  int rc = -1;
  if (unlim >= 0) {
    char buf[NC_MAX_NAME + 1] = {0};
    if (nc_inq_dimname(nc, unlim, buf) == NC_NOERR) {
      *name = buf;
      rc = 0;
    }
  }
  nc_close(nc);
  return rc;
}

int Backend::Serial::varSizes(const std::string& path, const std::string& var,
                              int sizes[4]) {
  int nc, v;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  int rc = sFindVarCI(nc, var, &v, nullptr);
  int nd = 0;
  if (rc == 0) {
    int dimids[NC_MAX_VAR_DIMS];
    nc_inq_varndims(nc, v, &nd);
    nc_inq_vardimid(nc, v, dimids);
    if (nd > 4) nd = 4;
    for (int k = 0; k < nd; ++k) {
      size_t len = 0;
      nc_inq_dimlen(nc, dimids[nd - 1 - k], &len);
      sizes[k] = (int)len;
    }
  }
  nc_close(nc);
  return rc == 0 ? nd : -1;
}

int Backend::Serial::readSlab(const std::string& path, const std::string& var,
                              const int start[4], const int count[4],
                              double* buf) {
  int nc, v;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  int rc = sFindVarCI(nc, var, &v, nullptr);
  if (rc == 0) {
    int nd = 0;
    nc_inq_varndims(nc, v, &nd);
    if (nd < 1 || nd > 4) rc = -2;
    if (rc == 0) {
      size_t s[4] = {0, 0, 0, 0}, c[4] = {1, 1, 1, 1};
      for (int k = 0; k < nd; ++k) {
        s[nd - 1 - k] = (size_t)(start[k] - 1);
        c[nd - 1 - k] = (size_t)count[k];
      }
      rc = (nc_get_vara_double(nc, v, s, c, buf) == NC_NOERR) ? 0 : -3;
    }
  }
  nc_close(nc);
  return rc;
}

int Backend::Serial::readPlain(const std::string& path, const std::string& var,
                               int timelevel, int n, double* buf) {
  int nc, v;
  if (nc_open(path.c_str(), NC_NOWRITE, &nc) != NC_NOERR) return -1;
  int rc = sFindVarCI(nc, var, &v, nullptr);
  if (rc == 0) {
    int nd = 0, unlim = -1, dimids[NC_MAX_VAR_DIMS];
    nc_inq_varndims(nc, v, &nd);
    nc_inq_vardimid(nc, v, dimids);
    nc_inq_unlimdim(nc, &unlim);
    bool has_t = false;
    for (int k = 0; k < nd; ++k)
      if (unlim >= 0 && dimids[k] == unlim) has_t = true;
    size_t s[4] = {0, 0, 0, 0}, c[4] = {1, 1, 1, 1};
    int k0 = 0;
    if (has_t && nd > 0) {
      s[0] = (size_t)(timelevel > 0 ? timelevel - 1 : 0);
      k0 = 1;
    }
    if (nd > k0) c[nd - 1] = (size_t)n;
    rc = (nc_get_vara_double(nc, v, s, c, buf) == NC_NOERR) ? 0 : -2;
  }
  nc_close(nc);
  return rc;
}

}  // namespace IO
}  // namespace TIM
