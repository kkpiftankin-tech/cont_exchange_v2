// ============================================================================
// combo_external_routing.cpp — F-09 MVP-5 (ADR-037). См. .hpp.
// ============================================================================

#include "app/combo_external_routing.hpp"

#include "cex/common/decimal.hpp"

namespace cex::matching::app {

SplitLegs SplitInternalExternal(const domain::MultiLegVectorOrder& order) {
  SplitLegs out;
  for (const auto& leg : order.legs) {
    if (leg.is_external()) {
      out.external.push_back(leg);
    } else {
      out.internal.push_back(leg);
    }
  }
  return out;
}

fob::execution::v1::ExecutionIntent BuildExternalIntent(const std::string& parent_order_id,
                                                        const std::string& batch_id,
                                                        const std::string& intent_id,
                                                        const domain::VectorLeg& leg) {
  fob::execution::v1::ExecutionIntent intent;
  intent.mutable_meta()->set_source("matching");
  intent.set_intent_id(intent_id);
  intent.set_batch_id(batch_id);
  intent.set_internal_order_id(leg.leg_id);  // линковка ExecutionReport → combo-нога
  intent.set_reason("combo_external_leg:" + parent_order_id);
  intent.mutable_instrument()->set_symbol(leg.instrument_symbol);

  // side из знака target_ratio: <0 → SELL, иначе BUY.
  const cex::common::Decimal zero{0, leg.target_ratio.scale};
  intent.set_side(cex::common::Decimal::cmp(leg.target_ratio, zero) < 0
                      ? fob::common::v1::SIDE_SELL
                      : fob::common::v1::SIDE_BUY);
  *intent.mutable_target_qty() = leg.remaining_qty().to_proto();

  for (const auto& v : leg.venue_preferences) intent.add_allowed_venues(v);
  return intent;
}

}  // namespace cex::matching::app
