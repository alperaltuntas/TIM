// TIM::IO backend seam over ParallelIO — the only TU that includes pio.h.
// Restricted to the PIOc_* subset PIO2 and SCORPIO share.

#include "tim_backend.hpp"

#include <mpi.h>
#include <pio.h>

#include <cstdio>

namespace TIM {
namespace IO {

SysId Backend::init(int /*placeholder*/, int niotasks, int stride) {
  // PROTOTYPE ASSUMPTION: compute pelist == MPI_COMM_WORLD (standalone MOM6);
  // production takes the component communicator.
  int iosysid = -1;
  int rc = PIOc_Init_Intracomm(MPI_COMM_WORLD, niotasks, stride, 0,
                               PIO_REARR_BOX, &iosysid);
  if (rc != PIO_NOERR) {
    std::fprintf(stderr, "TIM backend: Init_Intracomm rc=%d\n", rc);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  PIOc_set_iosystem_error_handling(iosysid, PIO_RETURN_ERROR, nullptr);
  return iosysid;
}

void Backend::finalize(SysId sys) { PIOc_finalize(sys); }

DecompId Backend::initDecomp(SysId sys, int ndims, const int* gdims,
                             const std::vector<long long>& dof) {
  static PIO_Offset dummy = 0;  // PIO rejects NULL maps even when empty
  std::vector<PIO_Offset> map(dof.begin(), dof.end());
  int ioid = -1, rearr = PIO_REARR_BOX;
  int rc = PIOc_InitDecomp(sys, PIO_DOUBLE, ndims, gdims, (int)map.size(),
                           map.empty() ? &dummy : map.data(), &ioid, &rearr,
                           nullptr, nullptr);
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

int Backend::setFrame(FileId f, VarId v, int frame) {
  return PIOc_setframe(f, v, frame);
}
int Backend::readDArray(FileId f, VarId v, DecompId d, long long n, double* buf) {
  static double dummy = 0.0;
  return PIOc_read_darray(f, v, d, (PIO_Offset)n, n ? buf : &dummy);
}
int Backend::writeDArray(FileId f, VarId v, DecompId d, long long n,
                         const double* buf) {
  static double dummy = 0.0;
  return PIOc_write_darray(f, v, d, (PIO_Offset)n,
                           const_cast<double*>(n ? buf : &dummy), nullptr);
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

}  // namespace IO
}  // namespace TIM
