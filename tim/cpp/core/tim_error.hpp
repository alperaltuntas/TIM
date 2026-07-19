#pragma once
/**
 * @file tim_error.hpp
 * @brief TIM's single fatal-error path.
 *
 * TIM error policy: no C++ exceptions (they are UB across the bind(C) boundary,
 * hang MPI collectives, and can't run on device). Unrecoverable conditions
 * abort via amrex::Abort with a descriptive message rather than returning a
 * status the caller can't act on.
 */

#include <string>

namespace TIM {

/// @brief Report an unrecoverable error and terminate; never returns.
///
/// TIM's fatal path for invalid input / precondition violations, matching
/// FMS's abort-based handling. Emitted as "TIM::<msg>".
/// @param msg Descriptive message.
[[noreturn]] void fatal(const std::string& msg);

}  // namespace TIM
