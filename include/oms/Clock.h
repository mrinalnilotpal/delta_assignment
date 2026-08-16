#pragma once

#include <chrono>

#include "oms/Types.h"

namespace oms {

// Default wall clock for the OMS and health model: monotonic nanoseconds since a
// steady epoch. Defined once here so it is not re-declared per translation unit;
// tests inject their own clock instead.
inline Timestamp steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace oms
