#pragma once
// TIM::IO::File — the deep module of TIM I/O (prototype design pass).
//
// One class stands in for FMS's plain-file/domain-file split, the netCDF
// define/data mode dance, record (unlimited-dim) management, case-insensitive
// variable lookup, file-vs-domain staggering reconciliation, and the
// decomposition bookkeeping of parallel reads and writes. Callers see
// open/define/read/write/close; everything else is hidden.
//
// RAII: move-only; the destructor closes. A File is either reading or
// writing, never both. All methods are collective over the iosystem.

#include "../core/tim_domain.hpp"
#include "tim_backend.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace TIM {
namespace IO {

class IoSystem;
class DofMap;

// A File borrows its IoSystem (which must outlive it); ensemble runs hand
// each member's Files that member's IoSystem.
class File {
 public:
  enum class Mode : int { Write = 0, Overwrite = 1, Append = 2 };

  // Factories. Absence and failure are the same non-event for reads:
  // "no File" (std::nullopt), never a half-open object.
  static std::optional<File> openForRead(IoSystem& sys, const std::string& path);
  static std::optional<File> create(IoSystem& sys, const std::string& path,
                                    int domainKey, const Decomp2D& domain,
                                    Mode mode);

  File(File&& o) noexcept { *this = std::move(o); }
  File& operator=(File&& o) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File() { close(); }

  int close();  // idempotent; also called by the destructor

  // ---- reading ----
  struct ReadInfo {
    int file_sx = 0;  // file's staggered axes carry the symmetric +1 point?
    int file_sy = 0;  //   (when 0 for a staggered var, the window's low-edge
                      //    line is left unfilled; the model halo-fills it)
  };
  // Fill the caller's staggered window buffer (Decomp2D::window(stagger)
  // layout, x fastest, then y, then k). The FILE decides the staggered axis
  // sizes (size-sniffed); the caller's window layout is fixed by the domain.
  int readDecomposed(const std::string& varname, int domainKey,
                     const Decomp2D& domain, Stagger stagger, int timelevel,
                     int nz, int nz2, double* buf, ReadInfo* info = nullptr);
  // The single deep read primitive: fill `buf` (map.count() doubles) from
  // `varname` at `timelevel` (1-based; 0 = no record) through an arbitrary
  // DofMap. readDecomposed is a thin wrapper over this + DofMapCache::fromDomain.
  // single: the variable is float (map must be a float/PIO_REAL decomp).
  int readDistributed(const std::string& varname, const DofMap& map,
                      int timelevel, double* buf, bool single = false);
  // Read the whole global field of `varname` at record `rec` (1-based; 0 = the
  // sole record) and replicate it on EVERY rank. `out` is sized to the full
  // global field, x fastest (then y, then z). Fields at/above the iosystem's
  // replicated-read threshold use a block-decomposed collective read +
  // MPI_Allgatherv; smaller fields use one broadcasting get_var. Collective.
  int readReplicated(const std::string& varname, int rec, double* out);
  // Whole (small) variable, replicated to every rank.
  int readPlain(const std::string& varname, int timelevel, int n, double* buf);
  // Replicated hyperslab; start/count given 1-based in Fortran dim order
  // (x,y,z,t); trailing entries beyond the variable's rank must be 1.
  int readSlab(const std::string& varname, const int start[4],
               const int count[4], double* buf);
  bool hasVar(const std::string& varname_ci) const;

  // ---- inquiry (read files) ----
  int numDimsInFile() const;
  int numVarsInFile() const;
  int numTimesInFile() const;                    // unlimited-dim length (0 if none)
  int timeValues(double* buf, int n) const;      // unlimited coordinate values
  int varNameAt(int index0, std::string* name) const;
  // Text attribute; false when the attribute (or variable) is absent.
  bool varAttText(const std::string& varname_ci, const std::string& att,
                  std::string* out) const;
  // Sizes in Fortran dim order (x first); returns ndims, or -1 if var absent.
  int varSizes(const std::string& varname_ci, int sizes[4]) const;

  // ---- writing (define phase; enddef is implicit at first write) ----
  enum class AxisKind : int { X = 0, Y = 1, Time = 2, Fixed = 3 };
  int defineAxis(const std::string& name, AxisKind kind, Stagger position,
                 int fixed_len, const std::string& units,
                 const std::string& longname, const std::string& cartesian,
                 std::optional<int> sense);
  // dims: axis names in Fortran order (x first); staggering, z-extents and
  // record-ness of the variable are derived from the named axes.
  // fill_missing: sets the variable's fill value (stored as _FillValue by the
  // classic formats) AND writes a matching missing_value attribute, both typed
  // by single_precision — the FMS diag-file convention.
  int defineVar(const std::string& name, const std::vector<std::string>& dims,
                const std::string& units, const std::string& longname,
                const std::string& standard_name, bool single_precision,
                const std::string& checksum_hex,
                std::optional<double> fill_missing = std::nullopt);
  int putGlobalAtt(const std::string& name, const std::string& value);
  // Extra attributes on an already-defined var/axis (define phase only).
  int putVarAtt(const std::string& varname, const std::string& att,
                const std::string& text);
  int putVarAtt(const std::string& varname, const std::string& att,
                const double* values, int n, bool as_float = false);
  int putVarAttInts(const std::string& varname, const std::string& att,
                    const int* values, int n);
  // Global text attribute of a read file; nullopt when absent.
  std::optional<std::string> globalAttText(const std::string& name) const;

  // ---- writing (data phase) ----
  int writeAxis(const std::string& name, const double* global_values, int n);
  // Window-buffer layout identical to readDecomposed's. Record management is
  // internal (FMS write_time_if_later semantics) when tstamp is provided.
  int writeDecomposed(const std::string& varname, const double* buf,
                      std::optional<double> tstamp);
  // The deep write primitive: symmetric partner of readDistributed. Writes
  // map.count() doubles (converted to float when single) through an arbitrary
  // DofMap; fill must equal the variable's own _FillValue.
  int writeDistributed(const std::string& varname, const DofMap& map,
                       const double* buf, double fill, bool single);
  int writePlain(const std::string& varname, const double* data, int n,
                 std::optional<double> tstamp);

  Stagger varStagger(const std::string& varname) const;  // registered vars
  int numTimes() const { return num_times_; }
  double fileTime() const { return file_time_; }

 private:
  File() = default;
  int endDef();
  int findVarCI(const std::string& name_ci, VarId* v,
                std::string* actual = nullptr) const;
  bool varHasUnlim(VarId v) const;
  int frameForTime(std::optional<double> tstamp);

  IoSystem* sys_ = nullptr;
  FileId id_ = -1;
  bool writable_ = false;
  bool in_def_ = false;
  int domain_key_ = -1;
  Decomp2D domain_;
  int num_times_ = 0;
  double file_time_ = 0.0;
  std::string unlim_name_;

  struct AxisInfo { AxisKind kind; Stagger position; int len; };
  struct VarInfo {
    Stagger stagger = Stagger::Center;
    int nz = 1, nz2 = 1;
    bool has_time = false;
    int ndims = 0;  // as defined (Fortran count, incl. the time axis)
    bool single = false;                        // float variable
    double fill = 9.9692099683868690e+36;       // the var's own fill value
  };
  std::map<std::string, AxisInfo> axes_;
  std::map<std::string, VarInfo> vars_;
};

}  // namespace IO
}  // namespace TIM
