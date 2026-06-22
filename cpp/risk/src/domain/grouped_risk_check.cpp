// ============================================================================
// grouped_risk_check.cpp — F-09 (T-F09-040). См. .hpp.
// ============================================================================

#include "domain/grouped_risk_check.hpp"

namespace cex::risk::domain {

namespace {

using cex::common::Decimal;

GroupPreTradeResult Reject(const std::string& reason) {
  return GroupPreTradeResult{RiskDecision::kReject, reason, "GROUPED_PRE_TRADE_REJECTED"};
}

}  // namespace

GroupPreTradeResult GroupPreTradeCheck(const GroupPreTradeInput& input,
                                       const GroupRiskConfig& config) {
  // 1) Лимит числа ног.
  if (static_cast<int>(input.legs.size()) > config.max_legs) {
    return Reject("max_legs_exceeded");
  }

  // 2) AC-F09-006: strict_atomic несовместима с external_compensating scope
  //    (внешняя компенсация не равна строгой атомарности).
  if (input.atomicity_policy == GroupAtomicityPolicy::kStrictAtomic &&
      input.atomicity_scope == GroupAtomicityScope::kExternalCompensating) {
    return Reject("strict_atomic_external_unsupported");
  }

  // 3) Лимит числа внешних ног + суммарный notional.
  int external_count = 0;
  Decimal total_notional = Decimal::zero();
  for (const auto& leg : input.legs) {
    if (leg.external) ++external_count;
    total_notional = Decimal::add(total_notional, leg.notional);
  }

  if (external_count > config.max_external_legs) {
    return Reject("max_external_legs_exceeded");
  }

  // max_total_notional == 0 трактуется как «без лимита».
  if (Decimal::cmp(config.max_total_notional, Decimal::zero()) > 0 &&
      Decimal::cmp(total_notional, config.max_total_notional) > 0) {
    return Reject("total_notional_exceeded");
  }

  return GroupPreTradeResult{RiskDecision::kAccept, "", ""};
}

}  // namespace cex::risk::domain
