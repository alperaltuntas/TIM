#pragma once
// TIM::Config — typed access to TIM's runtime configuration (prototype).
//
// Backed by AMReX ParmParse today; this header/TU pair is the ONLY place in
// TIM that touches the parser (the same one-includer rule as the PIO backend
// seam). The interface carries no AMReX types, so retargeting to TOML/YAML/
// CESM-namelist input is a one-file change, and TIM components never read
// configuration themselves — they take plain Options structs resolved at the
// composition boundary (the bind(C) adapter), which keeps them unit-testable
// and keeps per-instance configuration possible (ensembles).
//
// Sources, in precedence order:
//   1. environment override (when an env name is supplied) — quick experiments
//   2. the ParmParse table: keys like "tim.io.pio_ntasks", populated from the
//      optional ./TIM_input file (loaded once, lazily) and/or anything the
//      host application already fed AMReX
//   3. the caller's default

#include <string>

namespace TIM {

struct Config {
  // Loads ./TIM_input into the parse table once (no-op if absent or if AMReX
  // is not initialized yet). Called lazily by the getters.
  static void ensureLoaded();

  static int getInt(const std::string& key, int def,
                    const char* env_override = nullptr);
  static bool getBool(const std::string& key, bool def,
                      const char* env_override = nullptr);
  static std::string getString(const std::string& key, const std::string& def,
                               const char* env_override = nullptr);
};

}  // namespace TIM
