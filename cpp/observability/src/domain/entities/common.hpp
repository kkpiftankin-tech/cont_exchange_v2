#pragma once

#include <chrono>

namespace cex::observability::domain {

using Timestamp = std::chrono::time_point<std::chrono::system_clock>;

}  // namespace cex::observability::domain
