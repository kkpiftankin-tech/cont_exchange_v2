#pragma once

#include "common.hpp"

namespace cex::observability::domain {

enum class AlertCode { LowVenueHealthScore, CircuitBreakerOpen, LowLiquidityCurveConfidence };

enum class Severity { Info, Warning, Error, Critical };

enum class AlertStatus { Firing, Resolved };

struct Alert {
  std::string id;
  AlertCode code;
  std::unordered_map<std::string, std::string> labels;
  Severity severity;
  AlertStatus status;
};

}  // namespace cex::observability::domain
