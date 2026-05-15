#include "app/hedge_trigger_policy.hpp"

#include <utility>

namespace cex::matching::app {

namespace {

// calculate abs v
cex::common::Decimal AbsDecimal(const cex::common::Decimal& value) {
  if (cex::common::Decimal::cmp(value, cex::common::Decimal::zero()) >= 0) {
    return value;
  }
  return cex::common::Decimal::sub(cex::common::Decimal::zero(), value);
}

bool IsThresholdEnabled(const cex::common::Decimal& threshold) {
  return cex::common::Decimal::cmp(threshold, cex::common::Decimal::zero()) > 0;
}

bool HasValidReferenceMid(const fob::common::v1::Decimal& clearing_price) {
  const auto reference_mid = cex::common::Decimal::from_proto(clearing_price);
  return cex::common::Decimal::cmp(reference_mid, cex::common::Decimal::zero()) > 0;
}

}  // namespace 

HedgeTriggerPolicy::HedgeTriggerPolicy(HedgeTriggerConfig config)
    : config_(std::move(config)) {}

std::vector<HedgeTriggerDecision> HedgeTriggerPolicy::Evaluate(
    const std::vector<PositionSnapshot>& snapshots) const {
  std::vector<HedgeTriggerDecision> decisions;
  decisions.reserve(snapshots.size());

  for (const auto& snapshot : snapshots) {
    HedgeTriggerDecision decision;
    decision.snapshot = snapshot;
    decision.abs_qty = AbsDecimal(snapshot.net_qty);

    const auto& thresholds = ResolveThresholdForSymbol(snapshot.symbol);
    decision.qty_check_enabled = IsThresholdEnabled(thresholds.threshold_qty);
    decision.notional_check_enabled =
        IsThresholdEnabled(thresholds.threshold_notional) &&
        HasValidReferenceMid(snapshot.clearing_price);

    if (decision.qty_check_enabled) {
      decision.qty_threshold_exceeded =
          cex::common::Decimal::cmp(decision.abs_qty, thresholds.threshold_qty) > 0;
    }

    if (decision.notional_check_enabled) {
      const auto reference_mid = cex::common::Decimal::from_proto(snapshot.clearing_price);
      decision.abs_notional = cex::common::Decimal::mul(decision.abs_qty, reference_mid);
      decision.notional_threshold_exceeded =
          cex::common::Decimal::cmp(decision.abs_notional,
                                    thresholds.threshold_notional) > 0;
    }

    decision.triggered = decision.qty_threshold_exceeded ||
                         decision.notional_threshold_exceeded;
    decisions.push_back(std::move(decision));
  }

  return decisions;
}

HedgeTriggerConfig HedgeTriggerPolicy::DefaultConfig() {
  return {};
}

const HedgeTriggerThreshold& HedgeTriggerPolicy::ResolveThresholdForSymbol(
    const std::string& symbol) const {
  const auto it = config_.per_symbol_thresholds.find(symbol);
  if (it != config_.per_symbol_thresholds.end()) {
    return it->second;
  }
  return config_.default_thresholds;
}

}  // namespace cex::matching::app
