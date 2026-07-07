// TIM::Config over AMReX ParmParse — the only parser includer in TIM.

#include "tim_config.hpp"

#include <AMReX.H>
#include <AMReX_ParmParse.H>

#include <cstdlib>
#include <sys/stat.h>

namespace TIM {

void Config::ensureLoaded() {
  static bool loaded = false;
  if (loaded) return;
  if (!amrex::Initialized()) return;  // stay lazy until AMReX is up
  struct stat st;
  if (::stat("TIM_input", &st) == 0) amrex::ParmParse::addfile("TIM_input");
  loaded = true;
}

int Config::getInt(const std::string& key, int def, const char* env) {
  if (env) {
    if (const char* s = std::getenv(env)) return std::atoi(s);
  }
  ensureLoaded();
  int v = def;
  if (amrex::Initialized()) amrex::ParmParse().query(key.c_str(), v);
  return v;
}

bool Config::getBool(const std::string& key, bool def, const char* env) {
  return getInt(key, def ? 1 : 0, env) != 0;
}

std::string Config::getString(const std::string& key, const std::string& def,
                              const char* env) {
  if (env) {
    if (const char* s = std::getenv(env)) return s;
  }
  ensureLoaded();
  std::string v = def;
  if (amrex::Initialized()) amrex::ParmParse().query(key.c_str(), v);
  return v;
}

}  // namespace TIM
