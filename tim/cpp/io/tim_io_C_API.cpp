// C API adapter for the TIM I/O prototype: the extern "C" surface the Fortran
// seam calls, mapped onto the File/IoSystem/Decomp2D abstractions. Owns the
// bind(C)-level policies: the domain registry, the write-file handle table,
// and the READ-FILE CACHE — files are held open across the legacy stateless
// read calls (MOM opens/reads/closes per field; reopening a restart 50 times
// cost more than the reads at 768 ranks).

#include "../core/tim_domain.hpp"
#include "tim_file.hpp"
#include "tim_iosystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using TIM::Decomp2D;
using TIM::Stagger;
using TIM::IO::File;

std::vector<Decomp2D> g_domains;
std::map<std::string, File> g_read_cache;
std::vector<std::optional<File>> g_write_files;

bool debugOn() {
  static int on = -1;
  if (on < 0) {
    const char* s = std::getenv("TIM_IO_DEBUG");
    on = (s && s[0] == '1') ? 1 : 0;
  }
  return on == 1;
}

File* readFile(const std::string& path) {
  auto it = g_read_cache.find(path);
  if (it != g_read_cache.end()) return &it->second;
  auto f = File::openForRead(path);
  if (!f) return nullptr;
  return &g_read_cache.emplace(path, std::move(*f)).first->second;
}

Stagger stag(int code) { return static_cast<Stagger>(code); }

}  // namespace

extern "C" {

int tim_io_register_domain(int nig, int njg, int isc, int iec, int jsc,
                           int jec, int symmetric) {
  g_domains.emplace_back(nig, njg, isc, iec, jsc, jec, symmetric != 0);
  return (int)g_domains.size() - 1;
}

int tim_io_read_decomposed(const char* path, const char* varname,
                           int domain_handle, int stagger, int timelevel,
                           int nz, int nz2, double* buf, int* file_sx,
                           int* file_sy) {
  File* f = readFile(path);
  if (!f) return -1;
  File::ReadInfo info;
  int rc = f->readDecomposed(varname, domain_handle,
                             g_domains[(size_t)domain_handle], stag(stagger),
                             timelevel, nz, nz2, buf, &info);
  if (file_sx) *file_sx = info.file_sx;
  if (file_sy) *file_sy = info.file_sy;
  if (debugOn())
    std::fprintf(stderr, "TIM_IO read_dd  %s:%s stag=%d nz=%d rc=%d\n", path,
                 varname, stagger, nz, rc);
  return rc;
}

int tim_io_read_plain(const char* path, const char* varname, int timelevel,
                      int n, double* buf) {
  File* f = readFile(path);
  if (!f) return -1;
  if (debugOn())
    std::fprintf(stderr, "TIM_IO read_pl  %s:%s n=%d\n", path, varname, n);
  return f->readPlain(varname, timelevel, n, buf);
}

int tim_io_var_exists(const char* path, const char* varname) {
  File* f = readFile(path);
  return (f && f->hasVar(varname)) ? 1 : 0;
}

/* ---- write path ---- */

int tim_io_createfile(const char* path, int domain_handle, int mode) {
  auto f = File::create(path, domain_handle, g_domains[(size_t)domain_handle],
                        static_cast<File::Mode>(mode));
  if (!f) return -1;
  g_write_files.emplace_back(std::move(*f));
  return (int)g_write_files.size() - 1;
}

int tim_io_def_axis(int fh, const char* name, int kind, int position, int n,
                    const char* units, const char* longname,
                    const char* cartesian, int sense, int has_sense) {
  return g_write_files[(size_t)fh]->defineAxis(
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
  return g_write_files[(size_t)fh]->defineVar(name, dims, units, longname,
                                              std_name, pack > 1, checksum);
}

int tim_io_put_global_att(int fh, const char* name, const char* value) {
  return g_write_files[(size_t)fh]->putGlobalAtt(name, value);
}

int tim_io_write_axis(int fh, const char* name, const double* data, int n) {
  return g_write_files[(size_t)fh]->writeAxis(name, data, n);
}

int tim_io_var_stagger(int fh, const char* name) {
  return (int)g_write_files[(size_t)fh]->varStagger(name);
}

int tim_io_write_decomposed(int fh, const char* name, const double* buf,
                            double tstamp, int has_tstamp) {
  int rc = g_write_files[(size_t)fh]->writeDecomposed(
      name, buf, has_tstamp ? std::optional<double>(tstamp) : std::nullopt);
  if (debugOn())
    std::fprintf(stderr, "TIM_IO write_dd %s rc=%d\n", name, rc);
  return rc;
}

int tim_io_write_plain(int fh, const char* name, const double* data, int n,
                       double tstamp, int has_tstamp) {
  return g_write_files[(size_t)fh]->writePlain(
      name, data, n, has_tstamp ? std::optional<double>(tstamp) : std::nullopt);
}

int tim_io_closefile(int fh) {
  int rc = g_write_files[(size_t)fh]->close();
  g_write_files[(size_t)fh].reset();
  return rc;
}

int tim_io_file_num_times(int fh) {
  return g_write_files[(size_t)fh]->numTimes();
}
double tim_io_file_time(int fh) {
  return g_write_files[(size_t)fh]->fileTime();
}

void tim_io_finalize() {
  g_read_cache.clear();
  g_write_files.clear();
  g_domains.clear();
  TIM::IO::IoSystem::shutdown();
}

}  // extern "C"
