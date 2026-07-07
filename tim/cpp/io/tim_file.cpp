#include "tim_file.hpp"

#include "tim_decomp_cache.hpp"
#include "tim_iosystem.hpp"

#include <cctype>
#include <cstdio>
#include <vector>

namespace TIM {
namespace IO {

namespace {
bool ciEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
      return false;
  return true;
}
}  // namespace

File& File::operator=(File&& o) noexcept {
  if (this != &o) {
    close();
    sys_ = o.sys_;
    id_ = o.id_; o.id_ = -1;
    writable_ = o.writable_;
    in_def_ = o.in_def_;
    domain_key_ = o.domain_key_;
    domain_ = o.domain_;
    num_times_ = o.num_times_;
    file_time_ = o.file_time_;
    unlim_name_ = std::move(o.unlim_name_);
    axes_ = std::move(o.axes_);
    vars_ = std::move(o.vars_);
  }
  return *this;
}

std::optional<File> File::openForRead(IoSystem& sys, const std::string& path) {
  FileId id = Backend::openRead(sys.sys(), path);
  if (id < 0) return std::nullopt;
  File f;
  f.sys_ = &sys;
  f.id_ = id;
  return f;
}

std::optional<File> File::create(IoSystem& sys, const std::string& path,
                                 int domainKey, const Decomp2D& domain,
                                 Mode mode) {
  File f;
  f.sys_ = &sys;
  f.writable_ = true;
  f.domain_key_ = domainKey;
  f.domain_ = domain;
  if (mode == Mode::Append) {
    f.id_ = Backend::openWrite(sys.sys(), path);
    if (f.id_ < 0) return std::nullopt;
    // Recover record state so later timestamps append correctly.
    int ud = -1;
    Backend::unlimDim(f.id_, &ud);
    if (ud >= 0) {
      // dim name == coordinate var name by convention
      int nvars = 0;
      Backend::numVars(f.id_, &nvars);
      for (VarId v = 0; v < nvars; ++v) {
        int nd = 0, dimids[8];
        Backend::varNumDims(f.id_, v, &nd);
        Backend::varDimIds(f.id_, v, dimids);
        if (nd == 1 && dimids[0] == ud) {
          Backend::varName(f.id_, v, &f.unlim_name_);
          long long len = 0;
          Backend::dimLen(f.id_, ud, &len);
          f.num_times_ = (int)len;
          if (len > 0) {
            long long s = len - 1, c = 1;
            Backend::getVaraDouble(f.id_, v, &s, &c, 1, &f.file_time_);
          }
          break;
        }
      }
    }
    f.in_def_ = false;
  } else {
    f.id_ = Backend::create(sys.sys(), path);
    if (f.id_ < 0) return std::nullopt;
    f.in_def_ = true;
  }
  return f;
}

int File::close() {
  if (id_ < 0) return 0;
  endDef();
  int rc = Backend::close(id_);
  id_ = -1;
  return rc;
}

int File::endDef() {
  if (in_def_) { Backend::endDef(id_); in_def_ = false; }
  return 0;
}

int File::findVarCI(const std::string& name, VarId* v, std::string* actual) const {
  if (Backend::findVar(id_, name, v) == 0) {
    if (actual) *actual = name;
    return 0;
  }
  int nvars = 0;
  Backend::numVars(id_, &nvars);
  for (VarId cand = 0; cand < nvars; ++cand) {
    std::string nm;
    Backend::varName(id_, cand, &nm);
    if (ciEqual(nm, name)) {
      *v = cand;
      if (actual) *actual = nm;
      return 0;
    }
  }
  return -1;
}

bool File::hasVar(const std::string& name) const {
  VarId v;
  return findVarCI(name, &v) == 0;
}

bool File::varHasUnlim(VarId v) const {
  int ud = -1, nd = 0, dimids[8];
  Backend::unlimDim(id_, &ud);
  if (ud < 0) return false;
  Backend::varNumDims(id_, v, &nd);
  Backend::varDimIds(id_, v, dimids);
  for (int k = 0; k < nd; ++k)
    if (dimids[k] == ud) return true;
  return false;
}

int File::readDecomposed(const std::string& varname, int domainKey,
                         const Decomp2D& d, Stagger stagger, int timelevel,
                         int nz, int nz2, double* buf, ReadInfo* info) {
  VarId v;
  if (findVarCI(varname, &v) != 0) return -1;

  // The FILE decides the staggered axis sizes; size-sniff x (last dim) and
  // y (second-last) against the domain-implied sizes.
  int nd = 0, dimids[8];
  Backend::varNumDims(id_, v, &nd);
  Backend::varDimIds(id_, v, dimids);
  long long xlen = 0, ylen = 0;
  if (nd >= 1) Backend::dimLen(id_, dimids[nd - 1], &xlen);
  if (nd >= 2) Backend::dimLen(id_, dimids[nd - 2], &ylen);
  const bool want_sx = d.plusX(stagger), want_sy = d.plusY(stagger);
  const int fsx = (want_sx && xlen == (long long)d.nig() + 1) ? 1 : 0;
  const int fsy = (want_sy && ylen == (long long)d.njg() + 1) ? 1 : 0;
  if (info) { info->file_sx = fsx; info->file_sy = fsy; }
  Stagger fstag = Stagger::Center;
  if (fsx && fsy) fstag = Stagger::Corner;
  else if (fsx) fstag = Stagger::EastFace;
  else if (fsy) fstag = Stagger::NorthFace;

  if (varHasUnlim(v))
    Backend::setFrame(id_, v, timelevel > 0 ? timelevel - 1 : 0);

  // Read the disjoint components of the FILE's staggered window and scatter
  // into the caller's (domain-staggered) window buffer; the (want - file)
  // shift leaves the low-edge line untouched when the file lacks it.
  const Window wdw = d.window(stagger);
  const int wni = wdw.ni(), wnj = wdw.nj();
  const int shx = (want_sx ? 1 : 0) - fsx, shy = (want_sy ? 1 : 0) - fsy;

  auto& cache = sys_->decomps();
  Window comps[4];
  const int ncomp = d.readComponents(fstag, comps);
  std::vector<double> piece;
  for (int c = 0; c < ncomp; ++c) {
    const Window& cw = comps[c];
    DecompId ioid = cache.get(domainKey, d, fstag, nz, nz2,
                              DecompCache::Family::ReadComponent, c);
    piece.assign((size_t)cw.npts() * nz, 0.0);
    int rc = Backend::readDArray(id_, v, ioid, (long long)piece.size(),
                                 piece.data());
    if (rc != 0) {
      std::fprintf(stderr, "TIM File: read %s comp %d rc=%d\n", varname.c_str(),
                   c, rc);
      return rc;
    }
    const int ni = cw.ni(), nj = cw.nj();
    for (int k = 0; k < nz; ++k)
      for (int j = cw.js; j <= cw.je; ++j)
        for (int i = cw.is; i <= cw.ie; ++i)
          buf[(size_t)k * wni * wnj + (size_t)(j - wdw.js + shy) * wni +
              (i - wdw.is + shx)] =
              piece[(size_t)k * ni * nj + (size_t)(j - cw.js) * ni +
                    (i - cw.is)];
  }
  return 0;
}

int File::readPlain(const std::string& varname, int timelevel, int n,
                    double* buf) {
  VarId v;
  if (findVarCI(varname, &v) != 0) return -1;
  int nd = 0;
  Backend::varNumDims(id_, v, &nd);
  long long start[4] = {0, 0, 0, 0}, count[4] = {1, 1, 1, 1};
  int k = 0;
  if (varHasUnlim(v) && nd > 0) {
    start[0] = timelevel > 0 ? timelevel - 1 : 0;
    k = 1;
  }
  if (nd > k) count[nd - 1] = n;
  return Backend::getVaraDouble(id_, v, start, count, nd > 0 ? nd : 1, buf);
}

int File::defineAxis(const std::string& name, AxisKind kind, Stagger position,
                     int fixed_len, const std::string& units,
                     const std::string& longname, const std::string& cartesian,
                     std::optional<int> sense) {
  int len = fixed_len;
  if (kind == AxisKind::X) len = domain_.globalNx(position);
  if (kind == AxisKind::Y) len = domain_.globalNy(position);
  int dimid;
  int rc = Backend::defDim(id_, name, len, kind == AxisKind::Time, &dimid);
  if (rc != 0) return rc;
  VarId v;
  rc = Backend::defVar(id_, name, false, {dimid}, &v);
  if (rc != 0) return rc;
  if (!longname.empty()) Backend::putAttText(id_, v, "long_name", longname);
  if (!units.empty()) Backend::putAttText(id_, v, "units", units);
  if (!cartesian.empty()) Backend::putAttText(id_, v, "cartesian_axis", cartesian);
  if (sense) Backend::putAttInt(id_, v, "sense", *sense);
  axes_[name] = {kind, position, len};
  if (kind == AxisKind::Time) unlim_name_ = name;
  return 0;
}

int File::defineVar(const std::string& name, const std::vector<std::string>& dims,
                    const std::string& units, const std::string& longname,
                    const std::string& standard_name, bool single_precision,
                    const std::string& checksum_hex) {
  VarInfo vi{Stagger::Center, 1, 1, false};
  std::vector<int> dimids;
  bool sx = false, sy = false;
  std::vector<int> zlens;
  for (const auto& dn : dims) {
    int dimid;
    if (Backend::inqDimId(id_, dn, &dimid) != 0) return -1;
    dimids.push_back(dimid);
    auto it = axes_.find(dn);
    if (it == axes_.end()) return -1;
    const AxisInfo& a = it->second;
    if (a.kind == AxisKind::X && staggeredX(a.position)) sx = true;
    if (a.kind == AxisKind::Y && staggeredY(a.position)) sy = true;
    if (a.kind == AxisKind::Time) vi.has_time = true;
    if (a.kind == AxisKind::Fixed) zlens.push_back(a.len);
  }
  vi.stagger = sx && sy ? Stagger::Corner
               : sx    ? Stagger::EastFace
               : sy    ? Stagger::NorthFace
                       : Stagger::Center;
  for (int zl : zlens) vi.nz *= zl;
  vi.nz2 = (zlens.size() > 1) ? zlens.back() : 1;

  // netCDF wants dims slowest-first; MOM passes Fortran order (x first).
  std::vector<int> cdims(dimids.rbegin(), dimids.rend());
  VarId v;
  int rc = Backend::defVar(id_, name, single_precision, cdims, &v);
  if (rc != 0) return rc;
  Backend::defVarFill(id_, v, single_precision);
  if (!longname.empty()) Backend::putAttText(id_, v, "long_name", longname);
  if (!units.empty()) Backend::putAttText(id_, v, "units", units);
  if (!standard_name.empty())
    Backend::putAttText(id_, v, "standard_name", standard_name);
  if (!checksum_hex.empty())
    Backend::putAttText(id_, v, "checksum", checksum_hex);
  vars_[name] = vi;
  return 0;
}

int File::putGlobalAtt(const std::string& name, const std::string& value) {
  return Backend::putAttText(id_, Backend::globalAtts(), name, value);
}

int File::writeAxis(const std::string& name, const double* values, int n) {
  endDef();
  VarId v;
  if (Backend::findVar(id_, name, &v) != 0) return -1;
  long long s = 0, c = n;
  return Backend::putVaraDouble(id_, v, &s, &c, 1, values);
}

int File::frameForTime(std::optional<double> tstamp) {
  if (!tstamp) return -1;
  if (*tstamp > file_time_ || num_times_ == 0) {
    file_time_ = *tstamp;
    num_times_ += 1;
    if (!unlim_name_.empty()) {
      VarId uv;
      if (Backend::findVar(id_, unlim_name_, &uv) == 0) {
        long long s = num_times_ - 1, c = 1;
        double t = *tstamp;
        Backend::putVaraDouble(id_, uv, &s, &c, 1, &t);
      }
    }
  }
  return num_times_ - 1;
}

Stagger File::varStagger(const std::string& name) const {
  auto it = vars_.find(name);
  return it == vars_.end() ? Stagger::Center : it->second.stagger;
}

int File::writeDecomposed(const std::string& varname, const double* buf,
                          std::optional<double> tstamp) {
  auto it = vars_.find(varname);
  if (it == vars_.end()) return -1;
  const VarInfo& vi = it->second;
  endDef();
  VarId v;
  if (Backend::findVar(id_, varname, &v) != 0) return -1;
  const int frame = frameForTime(tstamp);
  if (vi.has_time && frame >= 0) Backend::setFrame(id_, v, frame);

  // Gather the write partition out of the caller's window buffer.
  const Window wdw = domain_.window(vi.stagger);
  const Window pw = domain_.writePartition(vi.stagger);
  const int wni = wdw.ni();
  std::vector<double> part((size_t)pw.npts() * vi.nz);
  const int ni = pw.ni(), nj = pw.nj();
  for (int k = 0; k < vi.nz; ++k)
    for (int j = pw.js; j <= pw.je; ++j)
      for (int i = pw.is; i <= pw.ie; ++i)
        part[(size_t)k * ni * nj + (size_t)(j - pw.js) * ni + (i - pw.is)] =
            buf[(size_t)k * wni * wdw.nj() + (size_t)(j - wdw.js) * wni +
                (i - wdw.is)];

  DecompId ioid = sys_->decomps().get(
      domain_key_, domain_, vi.stagger, vi.nz, vi.nz2,
      DecompCache::Family::WritePartition);
  int rc = Backend::writeDArray(id_, v, ioid, (long long)part.size(),
                                part.data());
  if (rc != 0)
    std::fprintf(stderr, "TIM File: write %s rc=%d\n", varname.c_str(), rc);
  return rc;
}

int File::writePlain(const std::string& varname, const double* data, int n,
                     std::optional<double> tstamp) {
  endDef();
  VarId v;
  if (Backend::findVar(id_, varname, &v) != 0) return -1;
  const int frame = frameForTime(tstamp);
  const auto it = vars_.find(varname);
  const bool has_t = it != vars_.end() && it->second.has_time;
  long long start[2] = {0, 0}, count[2] = {1, 1};
  int nd = 1;
  if (has_t && frame >= 0) {
    start[0] = frame; count[1] = n; nd = 2;
  } else {
    count[0] = n;
  }
  return Backend::putVaraDouble(id_, v, start, count, nd, data);
}

}  // namespace IO
}  // namespace TIM
