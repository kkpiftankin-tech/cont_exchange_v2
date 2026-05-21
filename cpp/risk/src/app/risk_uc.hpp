#pragma once
#include <mutex>
#include <string>
#include <unordered_map>

#include "fob/risk/v1/risk.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/venue/v1/venue.pb.h"
#include "infra/risk_alerts_publisher.hpp"
#include "cex/common/decimal.hpp"

namespace cex::risk::app {

// Application logic for Risk service (stateless-ish + kill switch state).
class RiskUseCases {
public:
  explicit RiskUseCases(infra::RiskAlertsPublisher publisher);

  fob::risk::v1::PreTradeCheckResponse
  CheckNewOrder(const fob::risk::v1::PreTradeCheckRequest &req);

  fob::risk::v1::KillSwitchResponse
  SetKillSwitch(const fob::risk::v1::KillSwitchRequest &req);

  void OnBatchResult(const fob::risk::v1::PostTradeUpdateRequest &req);

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

  infra::RiskAlertsPublisher publisher_;
};

} // namespace cex::risk::app
