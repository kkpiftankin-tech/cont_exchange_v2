#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "app/position_snapshot_calculator.hpp"
#include "cex/common/decimal.hpp"

namespace cex::matching::app {

struct HedgeTriggerThreshold {
  cex::common::Decimal threshold_qty{cex::common::Decimal::zero()};
  cex::common::Decimal threshold_notional{cex::common::Decimal::zero()};
};

struct HedgeTriggerConfig {
  HedgeTriggerThreshold default_thresholds{};
  std::unordered_map<std::string, HedgeTriggerThreshold> per_symbol_thresholds;
};

struct HedgeTriggerDecision {
  PositionSnapshot snapshot;
  bool triggered{false};
  cex::common::Decimal abs_qty{cex::common::Decimal::zero()};
  cex::common::Decimal abs_notional{cex::common::Decimal::zero()};
  bool qty_check_enabled{false};
  bool notional_check_enabled{false};
  bool qty_threshold_exceeded{false};
  bool notional_threshold_exceeded{false};
};

class HedgeTriggerPolicy {
 public:
  explicit HedgeTriggerPolicy(HedgeTriggerConfig config = DefaultConfig());

  std::vector<HedgeTriggerDecision> Evaluate(
      const std::vector<PositionSnapshot>& snapshots) const;

  static HedgeTriggerConfig DefaultConfig();

 private:
  const HedgeTriggerThreshold& ResolveThresholdForSymbol(
      const std::string& symbol) const;

  HedgeTriggerConfig config_;
};

}  // namespace cex::matching::app
