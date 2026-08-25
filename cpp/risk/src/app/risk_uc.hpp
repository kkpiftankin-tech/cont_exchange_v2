#pragma once
#include <mutex>
#include <string>
#include <unordered_map>

#include "fob/risk/v1/risk.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/venue/v1/venue.pb.h"
#include "infra/risk_alerts_publisher.hpp"
#include "infra/risk_snapshot_repository.hpp"
#include "cex/common/decimal.hpp"

namespace cex::risk::app {

// Application logic for Risk service (stateless-ish + kill switch state).
class RiskUseCases {
public:
  explicit RiskUseCases(infra::RiskAlertsPublisher publisher);

  // F-06 (T-F06-031/032): опциональный PG-репозиторий для buildRiskSnapshot /
  // GetRiskSnapshot. nullptr → snapshot-pipeline отключён (degraded mode).
  void SetSnapshotRepository(infra::RiskSnapshotRepository* repo) {
    snapshot_repo_ = repo;
  }

  fob::risk::v1::PreTradeCheckResponse
  CheckNewOrder(const fob::risk::v1::PreTradeCheckRequest &req);

  // F-12 DoD-3 / AC F12-8 (PR-F12-13). Pre-hedge risk check called by
  // Execution Planning before any ExecutionIntent is fulfilled. Five
  // checks in fixed order: PROVIDER_HALTED, NOTIONAL_EXCEEDED,
  // EXPOSURE_EXCEEDED, SLIPPAGE_EXCEEDED, VENUES_UNAVAILABLE. Decision
  // is ACCEPT or REJECT only (no RESIZE — hedges with residual exposure
  // create silent risk; operator must reissue smaller intent).
  fob::risk::v1::PreHedgeCheckResponse
  PreHedgeCheck(const fob::risk::v1::PreHedgeCheckRequest &req);

  fob::risk::v1::KillSwitchResponse
  SetKillSwitch(const fob::risk::v1::KillSwitchRequest &req);

  void OnBatchResult(const fob::risk::v1::PostTradeUpdateRequest &req);

  // F-06 (T-F06-031): построить RiskSnapshot для пользователя по BatchResult
  // (clear_prices как mark). Читает positions/accounts/risk_limits, считает
  // margin, пишет risk_snapshots, при margin_call/liquidation публикует
  // RiskAlert (T-F06-032). Возвращает false если запись снапшота не удалась.
  bool buildRiskSnapshot(const std::string &user_id,
                         const fob::matching::v1::BatchResult &batch);

  // F-06 (T-F06-032): отдаёт последний risk_snapshot для entity_id.
  fob::risk::v1::GetRiskSnapshotResponse
  GetRiskSnapshot(const fob::risk::v1::GetRiskSnapshotRequest &req);

  // F-09 (T-F09-040): эмитит RiskAlert GROUPED_PRE_TRADE_REJECTED в risk.alerts
  // при отклонении группового pre-trade check (для Observability / Operator UI).
  void PublishGroupedRejectAlert(const std::string &user_id, const std::string &reason);

  void CurveChecks(const fob::venue::v1::VenueLiquidityCurve &curve);
  void HealthChecks(const fob::venue::v1::VenueHealth &health);
  void OnExecutionReport(const fob::execution::v1::ExecutionReport &report);
  void OnSyntheticOrder(const fob::orders::v1::SyntheticFlowOrder &order);

private:
  enum class VenueHealthDecision {
    kAccept,
    kResize,
    kReject,
  };

  struct VenueHealthGateState {
    fob::venue::v1::VenueHealthStatus status{fob::venue::v1::VENUE_HEALTH_STATUS_UNSPECIFIED};
    fob::venue::v1::RoutingRecommendation routing{fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW};
    fob::venue::v1::CircuitBreakerState breaker{fob::venue::v1::CIRCUIT_BREAKER_STATE_UNSPECIFIED};
    double health_score{0.0};
  };

  bool is_halted_locked(const std::string &symbol) const;
  VenueHealthDecision EvaluateVenueHealthGateLocked() const;

  mutable std::mutex mu_;
  bool global_halt_{false};
  std::unordered_map<std::string, bool> instrument_halt_; // symbol->halt

  std::unordered_map<std::string, cex::common::Decimal> exposures_; // symbol->halt
  std::unordered_map<std::string, VenueHealthGateState> venue_health_gate_;

  // F-06: эмитит MARGIN_CALL / LIQUIDATION RiskAlert после INSERT снапшота.
  void PublishMarginAlert(const std::string &user_id,
                          const std::string &batch_id,
                          bool liquidation);

  infra::RiskAlertsPublisher publisher_;
  infra::RiskSnapshotRepository *snapshot_repo_{nullptr};  // optional, not owned
};

} // namespace cex::risk::app
