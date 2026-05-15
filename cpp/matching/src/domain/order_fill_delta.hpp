#pragma once

#include <string>

#include "cex/common/decimal.hpp"

namespace cex::matching::domain {

struct OrderFillDelta {
  std::string order_id;
  // Delta to cumulative fill in portfolio units.
  cex::common::Decimal executed_qty_delta{};
};

}  // namespace cex::matching::domain
