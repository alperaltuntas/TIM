/**
 * @file tim_error.cpp
 * @brief TIM's single fatal-error path (see tim_error.hpp).
 */

#include "tim_error.hpp"

#include <AMReX.H>

#include <cstdlib>

namespace TIM {

[[noreturn]] void fatal(const std::string& msg) {
  amrex::Abort("TIM::" + msg);
  std::abort();  // amrex::Abort is not marked [[noreturn]]; hold the contract
}

}  // namespace TIM
