#pragma once
// PROTOTYPE (throwaway) TIM I/O spine over ParallelIO — read path.
// Tactical code: exists to answer the exit questions in
// docs/tim_diag_io_plan.md ("Prototype pass"), not to be maintained.

#include <string>

namespace TIM {
namespace IO {

// Global compute-grid decomposition metadata for one MOM domain, 1-based
// global indices, h-point (center) extents. Staggered extents are derived.
struct DomainInfo {
  int nig = 0, njg = 0;          // global center-grid size
  int isc = 0, iec = 0;          // this rank's compute extent (1-based, global)
  int jsc = 0, jec = 0;
  int symmetric = 0;             // symmetric memory => corner/face +1 globals
};

// stagger codes (match MOM position flags semantically)
enum Stagger { CENTER = 0, EAST_FACE = 1, NORTH_FACE = 2, CORNER = 3 };

int registerDomain(const DomainInfo& d);   // -> handle (>=0)

// Read a domain-decomposed 2d/3d var into a contiguous compute-window buffer
// (x fastest, then y, then k; caller sized it from the staggered local extent).
// timelevel: 1-based record, or 0 for "no record/last-if-none".
// Returns 0 on success, nonzero PIO error code otherwise (message on stderr).
int readDecomposed(const std::string& path, const std::string& varname,
                   int domain_handle, int stagger, int timelevel, int nz,
                   int nz2, double* buf);

// Replicated read of a whole (small) 0d/1d var on all ranks.
int readPlain(const std::string& path, const std::string& varname,
              int timelevel, int n, double* buf);

// Case-insensitive variable existence probe (also returns actual name).
bool findVar(const std::string& path, const std::string& varname,
             std::string& actual_name);

void finalize();   // PIOc_finalize (idempotent)

}  // namespace IO
}  // namespace TIM
