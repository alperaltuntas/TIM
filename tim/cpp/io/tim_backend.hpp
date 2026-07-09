#pragma once
// TIM::IO backend seam (prototype design pass).
//
// THE ONLY TIM header/TU pair that touches ParallelIO. Everything above talks
// in these types; swapping PIO2 for SCORPIO (or another rearranging backend)
// means reimplementing this one file against the shared PIOc_* subset, with
// any signature drift absorbed here and nowhere else. No pio.h in this header.

#include <mpi.h>

#include <string>
#include <vector>

namespace TIM {
namespace IO {

// Opaque backend ids (PIO ints today; still ints for any conceivable backend).
using SysId = int;
using FileId = int;
using DecompId = int;
using VarId = int;

enum class IoType : int { PnetCDF = 0, NetCDF4Parallel = 1, NetCDFSerial = 2 };
enum class OpenMode : int { Read = 0, Write = 1 };
enum class CreateMode : int { Clobber = 0 };

struct Backend {
  // --- iosystem lifecycle (explicit communicator; ensemble-safe) ---
  static SysId init(MPI_Comm comm, int niotasks, int stride);
  static void finalize(SysId);

  // --- decompositions (dof: 1-based flat global offsets; empty ok) ---
  // single: float basetype — the backend requires the decomposition's type
  // to match the variable's, so single-precision vars get their own decomps.
  static DecompId initDecomp(SysId, int ndims, const int* gdims,
                             const std::vector<long long>& dof,
                             bool single = false);
  static void freeDecomp(SysId, DecompId);

  // --- files ---
  // Open for read; tries PnetCDF then NetCDF4Parallel. <0 on failure.
  static FileId openRead(SysId, const std::string& path);
  static FileId openWrite(SysId, const std::string& path);   // append-style
  static FileId create(SysId, const std::string& path);      // 64-bit offset
  static int close(FileId);
  static int endDef(FileId);

  // --- metadata ---
  static int defDim(FileId, const std::string& name, long long len_or_unlimited,
                    bool unlimited, int* dimid);
  static int inqDimId(FileId, const std::string& name, int* dimid);
  static int defVar(FileId, const std::string& name, bool single_precision,
                    const std::vector<int>& dimids_slowest_first, VarId* varid);
  // Prefill the variable with the netCDF default fill so cells no rank writes
  // (masked/eliminated tiles) match FMS-written files. Define mode only.
  static int defVarFill(FileId, VarId, bool single_precision);
  static int putAttText(FileId, VarId varid_or_global, const std::string& name,
                        const std::string& value);
  static int putAttInt(FileId, VarId varid_or_global, const std::string& name,
                       int value);
  static int putAttInts(FileId, VarId varid_or_global, const std::string& name,
                        const int* values, int n);
  // as_float stores the attribute as NC_FLOAT (matching single-precision vars).
  static int putAttDouble(FileId, VarId varid_or_global, const std::string& name,
                          const double* values, int n, bool as_float);
  // defVarFill with an explicit fill value (typed by single_precision); the
  // classic formats store it as the _FillValue attribute. Define mode only.
  static int defVarFillValue(FileId, VarId, bool single_precision, double value);
  static VarId globalAtts();

  // --- inquiry ---
  static int findVar(FileId, const std::string& name, VarId*);  // exact match
  static int varName(FileId, VarId, std::string*);
  static int numVars(FileId, int*);
  static int varNumDims(FileId, VarId, int*);
  static int varDimIds(FileId, VarId, int* dimids /*>=8*/);
  static int dimLen(FileId, int dimid, long long*);
  static int unlimDim(FileId, int* dimid_or_minus1);
  static int numDims(FileId, int*);
  // Text attribute of a var (or globalAtts()); nonzero rc when absent.
  static int getAttText(FileId, VarId, const std::string& name, std::string* out);

  // --- data ---
  static int setFrame(FileId, VarId, int frame0);
  static int readDArray(FileId, VarId, DecompId, long long n, double* buf);
  // fill: the VARIABLE's fill value (must match its _FillValue or the backend
  // rejects the write); single converts the buffer to float for float vars.
  static int writeDArray(FileId, VarId, DecompId, long long n,
                         const double* buf, double fill, bool single);
  // Contiguous hyperslab get/put (collective; result valid on all ranks).
  static int getVaraDouble(FileId, VarId, const long long start[],
                           const long long count[], int ndims, double* buf);

  // --- rank-INDEPENDENT serial access (plain netCDF) ---
  // FMS serves all non-domain file operations with per-rank serial reads;
  // MOM calls some of them on the root PE only (e.g. horizontal regridding
  // slabs), so these must never be collective. Case-insensitive var match.
  struct Serial {
    static bool fileExists(const std::string& path);
    static int findVarCI(const std::string& path, const std::string& var,
                         std::string* actual = nullptr);  // 0 if found
    static int fileInfo(const std::string& path, int* ndims, int* nvars,
                        int* ntimes);
    static int timeValues(const std::string& path, double* buf, int n);
    static int varNameAt(const std::string& path, int index0, std::string*);
    static int attText(const std::string& path, const std::string& var,
                       const std::string& att, std::string* out);
    // Numeric attribute (netCDF converts to double); nonzero when absent.
    static int attDouble(const std::string& path, const std::string& var,
                         const std::string& att, double* out, int n = 1);
    // Name of the unlimited dimension's coordinate variable.
    static int timeName(const std::string& path, std::string* name);
    static int varSizes(const std::string& path, const std::string& var,
                        int sizes[4]);  // Fortran order; returns ndims or <0
    // start/count 1-based Fortran dim order (x,y,z,t)
    static int readSlab(const std::string& path, const std::string& var,
                        const int start[4], const int count[4], double* buf);
    // whole small var (0d/1d), record-selected when the var has one
    static int readPlain(const std::string& path, const std::string& var,
                         int timelevel, int n, double* buf);
  };
  static int putVaraDouble(FileId, VarId, const long long start[],
                           const long long count[], int ndims, const double* buf);
};

}  // namespace IO
}  // namespace TIM
