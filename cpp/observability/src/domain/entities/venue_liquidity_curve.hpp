#pragma once

#include "common.hpp"

namespace cex::observability::domain {

struct VenueLiquidityCurve {
  std::string venue;
  std::string snapshot;
  double confidence;
  Timestamp timestamp;
};

}  // namespace cex::observability::domain
