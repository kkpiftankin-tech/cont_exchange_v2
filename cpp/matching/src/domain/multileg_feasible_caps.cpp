// ============================================================================
// multileg_feasible_caps.cpp — F-09 (T-F09-043). См. .hpp.
// ============================================================================

#include "domain/multileg_feasible_caps.hpp"

namespace cex::matching::domain {

namespace {

using cex::common::Decimal;

// Берёт candidate как новый минимум, если он строго меньше текущего cap,
// и запоминает связывающий фактор (для диагностики binding_factor).
void ConsiderCap(const Decimal& candidate, const char* name, Decimal& cap,
                 std::string& binding) {
  if (Decimal::cmp(candidate, cap) < 0) {
    cap = candidate;
    binding = name;
  }
}

}  // namespace

std::vector<FeasibleCap> ComputeFeasibleCaps(
    const MultiLegVectorOrder& order,
    [[maybe_unused]] const ReferencePrices& reference_prices) {
  std::vector<FeasibleCap> caps;
  caps.reserve(order.legs.size());

  for (const auto& leg : order.legs) {
    // База — оставшийся объём; далее последовательно зажимаем остальными капами.
    Decimal cap = leg.remaining_qty();
    std::string binding = "remaining";

    ConsiderCap(leg.q_rate, "rate", cap, binding);
    if (leg.q_liq.has_value()) ConsiderCap(*leg.q_liq, "liq", cap, binding);
    if (leg.q_risk.has_value()) ConsiderCap(*leg.q_risk, "risk", cap, binding);
    if (leg.q_venue.has_value()) ConsiderCap(*leg.q_venue, "venue", cap, binding);

    caps.push_back(FeasibleCap{leg.instrument_symbol, cap, binding});
  }

  return caps;
}

}  // namespace cex::matching::domain
