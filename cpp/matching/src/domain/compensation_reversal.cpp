// ============================================================================
// compensation_reversal.cpp — F-09 MVP-6 (ADR-039). См. .hpp.
// ============================================================================

#include "domain/compensation_reversal.hpp"

namespace cex::matching::domain {

std::vector<ReversalOrder> ComputeReversals(const std::vector<ReversalLeg>& internal_legs) {
  std::vector<ReversalOrder> out;
  for (const auto& leg : internal_legs) {
    // Разворачивать нечего, если объём нулевой/отрицательный.
    if (cex::common::Decimal::cmp(leg.filled_qty, cex::common::Decimal::zero()) <= 0) {
      continue;
    }
    out.push_back(ReversalOrder{leg.instrument_symbol, !leg.is_buy, leg.filled_qty});
  }
  return out;
}

}  // namespace cex::matching::domain
