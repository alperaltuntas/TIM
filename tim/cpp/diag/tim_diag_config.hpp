#pragma once
// TIM::Diag configuration model + classic diag_table parser (designed pass).
//
// The internal model (DiagConfig/FileSpec/FieldSpec) is the contract the diag
// manager consumes; the classic ASCII parser is one front end and a YAML
// front end can be added later without touching the model or its consumers.
// Parsing is rank-local (a tiny ASCII file); no MPI, no I/O backend, no AMReX
// — fully unit-testable in isolation.

#include "../core/tim_time.hpp"

#include <optional>
#include <string>
#include <vector>

namespace TIM {
namespace Diag {

enum class Reduction : int { None = 0, Mean, Min, Max, RMS, Pow, Diurnal };

struct FileSpec {
  std::string name;              // may contain %Nyr/%Nmo/%Ndy/%Nhr/%Nmi/%Nsc
  int output_freq = 0;           // >0 in units; 0 = every step; -1 = end of run
  TimeUnit output_freq_units = TimeUnit::Days;
  int file_format = 1;           // classic column; only 1 (netCDF) supported
  TimeUnit time_axis_units = TimeUnit::Days;
  std::string time_axis_name;    // conventionally contains "time"
  // optional trailing columns
  int new_file_freq = 0;         // 0 = no rollover
  TimeUnit new_file_freq_units = TimeUnit::Days;
  std::string start_time;        // "yyyy mm dd hh mm ss" or empty
  int file_duration = 0;
  TimeUnit file_duration_units = TimeUnit::Days;
};

struct FieldSpec {
  std::string module_name;       // e.g. "ocean_model"
  std::string field_name;        // model-side name
  std::string output_name;       // name in the file
  std::string file_name;         // FileSpec::name or "null" (discard)
  std::string time_sampling;     // classic column, always "all"
  Reduction reduction = Reduction::None;
  int pow_exponent = 1;          // for Reduction::Pow ("pow##")
  int diurnal_samples = 0;       // for Reduction::Diurnal ("diurnal##")
  std::string region;            // only "none" supported
  int packing = 1;               // 1 = double, 2 = float
};

struct DiagConfig {
  std::string title;
  DateFields base_date;
  std::vector<FileSpec> files;
  std::vector<FieldSpec> fields;

  const FileSpec* findFile(const std::string& name) const;
  // All fields destined for a given file, in table order.
  std::vector<const FieldSpec*> fieldsForFile(const std::string& name) const;
};

// Parses a classic ASCII diag_table. On failure returns nullopt and fills
// `error` with a "line N: what" message. Accepts both reduction spellings:
// the modern strings ("mean", "none", "min", ...) and the legacy logicals
// (.true. = mean, .false. = none). Fields naming unknown files (other than
// "null") are an error, as in FMS.
std::optional<DiagConfig> parseClassicDiagTable(const std::string& path,
                                                std::string* error);

}  // namespace Diag
}  // namespace TIM
