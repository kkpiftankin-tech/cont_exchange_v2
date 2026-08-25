#pragma once
// ============================================================================
// grouped_risk_check.hpp — F-09 (T-F09-040). Risk domain (pure).
//
// Групповой pre-trade risk: проверяет combo-группу как единое целое
// (документ F-09 v2 §3.9): max legs, суммарный notional, max внешних ног и
// несовместимость strict_atomic + external_compensating (AC-F09-006). Чистая
// функция (без proto/IO) → решение ACCEPT/REJECT + alert_type для risk.alerts.
// ============================================================================

#include <string>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::risk::domain {

enum class GroupAtomicityPolicy { kStrictAtomic, kScalableAtomic, kBestEffort,
                                  kSequentialFallback, kExternalCompensating };
enum class GroupAtomicityScope { kInternalBatch, kVenueNative,
                                 kExternalCompensating, kNone };

/// Решение (зеркало fob.risk.v1.RiskDecision).
enum class RiskDecision { kAccept, kReject, kResize, kHalt };

struct GroupRiskLeg {
  std::string instrument_symbol;
  cex::common::Decimal notional{};  ///< |qty * price| в quote currency
  bool external{false};             ///< нога требует внешней venue (Fâ12)
};

struct GroupPreTradeInput {
  std::vector<GroupRiskLeg> legs;
  GroupAtomicityPolicy atomicity_policy{GroupAtomicityPolicy::kBestEffort};
  GroupAtomicityScope atomicity_scope{GroupAtomicityScope::kInternalBatch};
};

struct GroupRiskConfig {
  cex::common::Decimal max_total_notional{};  ///< 0 = без лимита
  int max_legs{8};
  int max_external_legs{4};
};

struct GroupPreTradeResult {
  RiskDecision decision{RiskDecision::kAccept};
  std::string reason;      ///< пусто при ACCEPT
  std::string alert_type;  ///< "GROUPED_PRE_TRADE_REJECTED" при REJECT, иначе пусто
};

/// Групповой pre-trade check. Детерминирован относительно (input, config).
[[nodiscard]] GroupPreTradeResult GroupPreTradeCheck(const GroupPreTradeInput& input,
                                                     const GroupRiskConfig& config);

}  // namespace cex::risk::domain
