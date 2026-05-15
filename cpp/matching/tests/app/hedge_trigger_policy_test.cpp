#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "app/hedge_trigger_policy.hpp"

namespace {

using cex::common::Decimal;
using cex::matching::app::HedgeTriggerConfig;
using cex::matching::app::HedgeTriggerPolicy;
using cex::matching::app::HedgeTriggerThreshold;
using cex::matching::app::PositionSnapshot;

fob::common::v1::Decimal ProtoDec(const int64_t units, const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

Decimal Dec(const int64_t units, const int32_t scale = 0) {
  return Decimal{units, scale};
}

bool Expect(const bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

PositionSnapshot MakeSnapshot(const std::string& symbol,
                              const Decimal& net_qty,
                              const fob::common::v1::Decimal& clearing_price =
                                  ProtoDec(10000, 0)) {
  PositionSnapshot snapshot;
  snapshot.provider_id = "provider-1";
  snapshot.symbol = symbol;
  snapshot.net_qty = net_qty;
  snapshot.batch_id = "batch-1";
  snapshot.clearing_price = clearing_price;
  snapshot.timestamp.set_seconds(1700000000);
  return snapshot;
}

HedgeTriggerConfig BaseConfig() {
  HedgeTriggerConfig config;
  config.default_thresholds.threshold_qty = Dec(10, 0);
  config.default_thresholds.threshold_notional = Dec(100000, 0);
  return config;
}

bool TestNoTriggerWhenQtyBelowThreshold() {
  const HedgeTriggerPolicy policy(BaseConfig());
  const std::vector<PositionSnapshot> snapshots = {
      MakeSnapshot("BTC/USDT", Dec(9, 0), ProtoDec(10000, 0))};

  const auto decisions = policy.Evaluate(snapshots);

  bool ok = true;
  ok = Expect(decisions.size() == 1, "qty below: one decision") && ok;
  if (decisions.size() == 1) {
    ok = Expect(!decisions[0].triggered,
                "qty below threshold should not trigger") && ok;
    ok = Expect(!decisions[0].qty_threshold_exceeded,
                "qty below threshold should not exceed qty check") && ok;
  }
  return ok;
}

bool TestTriggerWhenQtyAboveThreshold() {
  const HedgeTriggerPolicy policy(BaseConfig());
  const std::vector<PositionSnapshot> snapshots = {
      MakeSnapshot("BTC/USDT", Dec(11, 0), ProtoDec(1000, 0))};

  const auto decisions = policy.Evaluate(snapshots);

  bool ok = true;
  ok = Expect(decisions.size() == 1, "qty above: one decision") && ok;
  if (decisions.size() == 1) {
    ok = Expect(decisions[0].triggered,
                "qty above threshold should trigger") && ok;
    ok = Expect(decisions[0].qty_threshold_exceeded,
                "qty above threshold should exceed qty check") && ok;
  }
  return ok;
}

bool TestTriggerWhenNotionalAboveThreshold() {
  const HedgeTriggerPolicy policy(BaseConfig());
  const std::vector<PositionSnapshot> snapshots = {
      MakeSnapshot("BTC/USDT", Dec(2, 0), ProtoDec(60000, 0))};

  const auto decisions = policy.Evaluate(snapshots);

  bool ok = true;
  ok = Expect(decisions.size() == 1, "notional above: one decision") && ok;
  if (decisions.size() == 1) {
    ok = Expect(decisions[0].triggered,
                "notional above threshold should trigger") && ok;
    ok = Expect(decisions[0].notional_threshold_exceeded,
                "notional above threshold should exceed notional check") && ok;
  }
  return ok;
}

bool TestNegativeNetQtyUsesAbsoluteValue() {
  const HedgeTriggerPolicy policy(BaseConfig());
  const std::vector<PositionSnapshot> snapshots = {
      MakeSnapshot("BTC/USDT", Dec(-11, 0), ProtoDec(1000, 0))};

  const auto decisions = policy.Evaluate(snapshots);

  bool ok = true;
  ok = Expect(decisions.size() == 1, "negative qty: one decision") && ok;
  if (decisions.size() == 1) {
    ok = Expect(decisions[0].triggered,
                "negative qty should be compared by abs and trigger") && ok;
    ok = Expect(Decimal::cmp(decisions[0].abs_qty, Dec(11, 0)) == 0,
                "abs_qty must be positive absolute value") && ok;
  }
  return ok;
}

bool TestPerSymbolThresholdOverridesDefault() {
  HedgeTriggerConfig config = BaseConfig();
  config.per_symbol_thresholds["BTC/USDT"] = HedgeTriggerThreshold{
      .threshold_qty = Dec(20, 0),
      .threshold_notional = Dec(500000, 0),
  };

  const HedgeTriggerPolicy policy(config);
  const std::vector<PositionSnapshot> snapshots = {
      MakeSnapshot("BTC/USDT", Dec(15, 0), ProtoDec(1000, 0))};

  const auto decisions = policy.Evaluate(snapshots);

  bool ok = true;
  ok = Expect(decisions.size() == 1, "per-symbol override: one decision") && ok;
  if (decisions.size() == 1) {
    ok = Expect(!decisions[0].triggered,
                "per-symbol qty threshold override must be applied") && ok;
  }
  return ok;
}

bool TestUsesDefaultThresholdForUnknownSymbol() {
  HedgeTriggerConfig config = BaseConfig();
  config.per_symbol_thresholds["BTC/USDT"] = HedgeTriggerThreshold{
      .threshold_qty = Dec(20, 0),
      .threshold_notional = Dec(500000, 0),
  };

  const HedgeTriggerPolicy policy(config);
  const std::vector<PositionSnapshot> snapshots = {
      MakeSnapshot("ETH/USDT", Dec(15, 0), ProtoDec(1000, 0))};

  const auto decisions = policy.Evaluate(snapshots);

  bool ok = true;
  ok = Expect(decisions.size() == 1, "default fallback: one decision") && ok;
  if (decisions.size() == 1) {
    ok = Expect(decisions[0].triggered,
                "unknown symbol should use default threshold and trigger") && ok;
  }
  return ok;
}

bool TestMissingOrZeroClearingPriceDisablesNotionalCheck() {
  HedgeTriggerConfig config;
  config.default_thresholds.threshold_qty = Dec(0, 0);
  config.default_thresholds.threshold_notional = Dec(100, 0);

  const HedgeTriggerPolicy policy(config);
  const std::vector<PositionSnapshot> snapshots = {
      MakeSnapshot("BTC/USDT", Dec(10, 0), ProtoDec(0, 0)),
      MakeSnapshot("ETH/USDT", Dec(10, 0), fob::common::v1::Decimal{})};

  const auto decisions = policy.Evaluate(snapshots);

  bool ok = true;
  ok = Expect(decisions.size() == 2, "clearing price edge: two decisions") && ok;
  if (decisions.size() == 2) {
    ok = Expect(!decisions[0].triggered,
                "zero clearing price should not trigger notional check") && ok;
    ok = Expect(!decisions[1].triggered,
                "missing clearing price should not trigger notional check") && ok;
    ok = Expect(!decisions[0].notional_check_enabled,
                "zero clearing price disables notional check") && ok;
    ok = Expect(!decisions[1].notional_check_enabled,
                "missing clearing price disables notional check") && ok;
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestNoTriggerWhenQtyBelowThreshold() && ok;
  ok = TestTriggerWhenQtyAboveThreshold() && ok;
  ok = TestTriggerWhenNotionalAboveThreshold() && ok;
  ok = TestNegativeNetQtyUsesAbsoluteValue() && ok;
  ok = TestPerSymbolThresholdOverridesDefault() && ok;
  ok = TestUsesDefaultThresholdForUnknownSymbol() && ok;
  ok = TestMissingOrZeroClearingPriceDisablesNotionalCheck() && ok;
  return ok ? 0 : 1;
}
