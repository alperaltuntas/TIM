// C API adapter for the TIM I/O prototype: the extern "C" surface the Fortran
// seam calls, mapped onto IoContext/File/IoSystem. The adapter owns exactly
// ONE explicitly-managed IoContext per component: tim_io_init(fcomm) creates
// it on the component communicator (MOM_infra_init passes its localcomm —
// ensemble-safe), tim_io_finalize destroys it before MPI_Finalize. There are
// no singletons in the C++ layer; multiple contexts (multiple components in
// one executable) only need this adapter to grow a handle, not any redesign.

#include "../core/tim_config.hpp"
#include "../core/tim_domain.hpp"
#include "tim_backend.hpp"
#include "tim_io_C_API_internal.hpp"
#include "tim_io_context.hpp"

#include <mpi.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using TIM::Decomp2D;
using TIM::Stagger;
using TIM::IO::File;
using TIM::IO::IoContext;

std::unique_ptr<IoContext> g_ctx;  // owned by tim_io_init/tim_io_finalize

TIM::IO::IoSystem::Options resolveOptions() {
  // Composition boundary: configuration is resolved HERE (Config = the sole
  // parser includer) and injected as plain options; TIM classes stay
  // config-free and unit-testable.
  TIM::IO::IoSystem::Options o;
  o.niotasks = TIM::Config::getInt("tim.io.pio_ntasks", -1, "TIM_PIO_NTASKS");
  return o;
}

IoContext& ctx() {
  if (!g_ctx) {
    // Fallback for standalone tools that skip tim_io_init.
    std::fprintf(stderr,
                 "tim_io: WARNING: tim_io_init was not called; using "
                 "MPI_COMM_WORLD\n");
    g_ctx = std::make_unique<IoContext>(MPI_COMM_WORLD, resolveOptions());
  }
  return *g_ctx;
}

bool debugOn() {
  static int on = -1;
  if (on < 0) on = TIM::Config::getBool("tim.io.debug", false, "TIM_IO_DEBUG") ? 1 : 0;
  return on == 1;
}

Stagger stag(int code) { return static_cast<Stagger>(code); }

}  // namespace

namespace TIM {
namespace IO {
IoContext& currentIoContext() { return ctx(); }
}  // namespace IO
}  // namespace TIM

extern "C" {

void tim_io_init(int fcomm) {
  if (g_ctx) return;
  g_ctx = std::make_unique<IoContext>(
      fcomm < 0 ? MPI_COMM_WORLD : MPI_Comm_f2c((MPI_Fint)fcomm),
      resolveOptions());
}

/* Configuration lookup for the Fortran seam (env name may be empty). */
int tim_io_cfg_bool(const char* key, const char* env, int def) {
  return TIM::Config::getBool(key, def != 0,
                              (env && env[0]) ? env : nullptr) ? 1 : 0;
}

void tim_io_finalize() { g_ctx.reset(); }

int tim_io_register_domain(int nig, int njg, int isc, int iec, int jsc,
                           int jec, int symmetric) {
  return ctx().registerDomain(
      Decomp2D(nig, njg, isc, iec, jsc, jec, symmetric != 0));
}

int tim_io_read_decomposed(const char* path, const char* varname,
                           int domain_handle, int stagger, int timelevel,
                           int nz, int nz2, double* buf, int* file_sx,
                           int* file_sy) {
  File* f = ctx().readFile(path);
  if (!f) return -1;
  File::ReadInfo info;
  int rc = f->readDecomposed(varname, domain_handle, ctx().domain(domain_handle),
                             stag(stagger), timelevel, nz, nz2, buf, &info);
  if (file_sx) *file_sx = info.file_sx;
  if (file_sy) *file_sy = info.file_sy;
  if (debugOn())
    std::fprintf(stderr, "TIM_IO read_dd  %s:%s stag=%d nz=%d rc=%d\n", path,
                 varname, stagger, nz, rc);
  return rc;
}

/* Path-based queries, plain reads and slab reads are RANK-INDEPENDENT
   (Backend::Serial): FMS serves all non-domain file access with per-rank
   serial netCDF, and MOM calls several of these on the root PE only (e.g.
   horizontal-regridding slabs) — a collective here deadlocks. Only the
   decomposed reads/writes (inherently collective) go through PIO. */

using TIM::IO::Backend;

int tim_io_read_plain(const char* path, const char* varname, int timelevel,
                      int n, double* buf) {
  if (debugOn())
    std::fprintf(stderr, "TIM_IO read_pl  %s:%s n=%d\n", path, varname, n);
  return Backend::Serial::readPlain(path, varname, timelevel, n, buf);
}

int tim_io_var_exists(const char* path, const char* varname) {
  return Backend::Serial::findVarCI(path, varname) == 0 ? 1 : 0;
}

int tim_io_file_exists(const char* path) {
  return Backend::Serial::fileExists(path) ? 1 : 0;
}

int tim_io_file_info(const char* path, int* ndims, int* nvars, int* ntimes) {
  return Backend::Serial::fileInfo(path, ndims, nvars, ntimes);
}

int tim_io_file_times(const char* path, double* buf, int n) {
  return Backend::Serial::timeValues(path, buf, n);
}

int tim_io_file_var_name(const char* path, int index1, char* out, int maxlen) {
  std::string name;
  if (Backend::Serial::varNameAt(path, index1 - 1, &name) != 0) return -1;
  std::snprintf(out, (size_t)maxlen, "%s", name.c_str());
  return 0;
}

int tim_io_var_att(const char* path, const char* varname, const char* att,
                   char* out, int maxlen) {
  std::string val;
  if (Backend::Serial::attText(path, varname, att, &val) != 0) return -1;
  std::snprintf(out, (size_t)maxlen, "%s", val.c_str());
  return 0;
}

int tim_io_var_sizes(const char* path, const char* varname, int sizes[4]) {
  return Backend::Serial::varSizes(path, varname, sizes);
}

int tim_io_read_slab(const char* path, const char* varname, const int start[4],
                     const int nread[4], double* buf) {
  if (debugOn())
    std::fprintf(stderr, "TIM_IO read_sl  %s:%s [%d,%d,%d,%d]+[%d,%d,%d,%d]\n",
                 path, varname, start[0], start[1], start[2], start[3],
                 nread[0], nread[1], nread[2], nread[3]);
  return Backend::Serial::readSlab(path, varname, start, nread, buf);
}

/* ---- write path ---- */

int tim_io_createfile(const char* path, int domain_handle, int mode) {
  auto f = File::create(ctx().sys(), path, domain_handle,
                        ctx().domain(domain_handle),
                        static_cast<File::Mode>(mode));
  if (!f) return -1;
  return ctx().addWriteFile(std::move(*f));
}

int tim_io_def_axis(int fh, const char* name, int kind, int position, int n,
                    const char* units, const char* longname,
                    const char* cartesian, int sense, int has_sense) {
  return ctx().writeFile(fh).defineAxis(
      name, static_cast<File::AxisKind>(kind), stag(position), n, units,
      longname, cartesian,
      has_sense ? std::optional<int>(sense) : std::nullopt);
}

int tim_io_def_var(int fh, const char* name, const char* dims_joined,
                   const char* units, const char* longname,
                   const char* std_name, int pack, const char* checksum) {
  std::vector<std::string> dims;
  const std::string joined = dims_joined;
  size_t p = 0;
  while (p < joined.size()) {
    size_t q = joined.find('\n', p);
    if (q == std::string::npos) q = joined.size();
    if (q > p) dims.push_back(joined.substr(p, q - p));
    p = q + 1;
  }
  return ctx().writeFile(fh).defineVar(name, dims, units, longname, std_name,
                                       pack > 1, checksum);
}

int tim_io_put_global_att(int fh, const char* name, const char* value) {
  return ctx().writeFile(fh).putGlobalAtt(name, value);
}

int tim_io_write_axis(int fh, const char* name, const double* data, int n) {
  return ctx().writeFile(fh).writeAxis(name, data, n);
}

int tim_io_var_stagger(int fh, const char* name) {
  return (int)ctx().writeFile(fh).varStagger(name);
}

int tim_io_write_decomposed(int fh, const char* name, const double* buf,
                            double tstamp, int has_tstamp) {
  int rc = ctx().writeFile(fh).writeDecomposed(
      name, buf, has_tstamp ? std::optional<double>(tstamp) : std::nullopt);
  if (debugOn()) std::fprintf(stderr, "TIM_IO write_dd %s rc=%d\n", name, rc);
  return rc;
}

int tim_io_write_plain(int fh, const char* name, const double* data, int n,
                       double tstamp, int has_tstamp) {
  return ctx().writeFile(fh).writePlain(
      name, data, n, has_tstamp ? std::optional<double>(tstamp) : std::nullopt);
}

int tim_io_closefile(int fh) {
  ctx().closeWriteFile(fh);
  return 0;
}

int tim_io_file_num_times(int fh) { return ctx().writeFile(fh).numTimes(); }
double tim_io_file_time(int fh) { return ctx().writeFile(fh).fileTime(); }

}  // extern "C"
