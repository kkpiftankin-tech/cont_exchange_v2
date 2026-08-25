// ============================================================================
// grouped_risk_check_test.cpp — F-09 (T-F09-040). Hand-rolled harness.
// strict+external reject (AC-F09-006), notional>limit reject, max-legs reject,
// basket within limits → accept.
// ============================================================================

#include <iostream>
#include <string>

#include "domain/grouped_risk_check.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::risk::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::GroupRiskLeg Leg(const std::string& sym, std::int64_t notional, bool external = false) {
  return d::GroupRiskLeg{sym, Decimal{notional, 0}, external};
}

d::GroupRiskConfig Cfg(std::int64_t max_notional, int max_legs = 8, int max_ext = 4) {
  return d::GroupRiskConfig{Decimal{max_notional, 0}, max_legs, max_ext};
}

}  // namespace

int main() {
  bool ok = true;

  // 1) strict_atomic + external_compensating → REJECT (AC-F09-006).
  {
    d::GroupPreTradeInput in;
    in.atomicity_policy = d::GroupAtomicityPolicy::kStrictAtomic;
    in.atomicity_scope = d::GroupAtomicityScope::kExternalCompensating;
    in.legs = {Leg("BTCUSDT", 100), Leg("ETHUSDT", 100)};
    const auto r = d::GroupPreTradeCheck(in, Cfg(0));
    ok = expect(r.decision == d::RiskDecision::kReject, "strict+external → REJECT") && ok;
    ok = expect(r.reason == "strict_atomic_external_unsupported", "reason set") && ok;
    ok = expect(r.alert_type == "GROUPED_PRE_TRADE_REJECTED", "alert_type set") && ok;
  }

  // 2) total notional > limit → REJECT.
  {
    d::GroupPreTradeInput in;
    in.atomicity_policy = d::GroupAtomicityPolicy::kScalableAtomic;
    in.atomicity_scope = d::GroupAtomicityScope::kInternalBatch;
    in.legs = {Leg("BTCUSDT", 600), Leg("ETHUSDT", 600)};  // sum 1200
    const auto r = d::GroupPreTradeCheck(in, Cfg(1000));    // limit 1000
    ok = expect(r.decision == d::RiskDecision::kReject && r.reason == "total_notional_exceeded",
                "notional>limit → REJECT") && ok;
  }

  // 3) max legs exceeded → REJECT.
  {
    d::GroupPreTradeInput in;
    in.atomicity_policy = d::GroupAtomicityPolicy::kBestEffort;
    in.legs = {Leg("A", 1), Leg("B", 1), Leg("C", 1)};
    const auto r = d::GroupPreTradeCheck(in, Cfg(0, /*max_legs=*/2));
    ok = expect(r.decision == d::RiskDecision::kReject && r.reason == "max_legs_exceeded",
                "legs>max → REJECT") && ok;
  }

  // 4) max external legs exceeded → REJECT.
  {
    d::GroupPreTradeInput in;
    in.legs = {Leg("A", 1, /*external=*/true), Leg("B", 1, true)};
    const auto r = d::GroupPreTradeCheck(in, Cfg(0, 8, /*max_ext=*/1));
    ok = expect(r.decision == d::RiskDecision::kReject && r.reason == "max_external_legs_exceeded",
                "external>max → REJECT") && ok;
  }

  // 5) basket 2 ноги в пределах → ACCEPT.
  {
    d::GroupPreTradeInput in;
    in.atomicity_policy = d::GroupAtomicityPolicy::kScalableAtomic;
    in.atomicity_scope = d::GroupAtomicityScope::kInternalBatch;
    in.legs = {Leg("BTCUSDT", 300), Leg("ETHUSDT", 200)};  // sum 500
    const auto r = d::GroupPreTradeCheck(in, Cfg(1000));
    ok = expect(r.decision == d::RiskDecision::kAccept, "within limits → ACCEPT") && ok;
    ok = expect(r.reason.empty() && r.alert_type.empty(), "accept: no reason/alert") && ok;
  }

  if (ok) { std::cout << "grouped_risk_check_test: ALL PASSED\n"; return 0; }
  return 1;
}
