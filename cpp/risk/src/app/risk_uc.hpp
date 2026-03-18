#pragma once
#include <mutex>
#include <string>
#include <unordered_map>

#include "fob/risk/v1/risk.pb.h"
#include "infra/risk_alerts_publisher.hpp"

namespace cex::risk::app {

// Application logic for Risk service (stateless-ish + kill switch state).
class RiskUseCases {
 public:
  explicit RiskUseCases(infra::RiskAlertsPublisher publisher);

  fob::risk::v1::PreTradeCheckResponse CheckNewOrder(
      const fob::risk::v1::PreTradeCheckRequest& req);

  fob::risk::v1::KillSwitchResponse SetKillSwitch(
      const fob::risk::v1::KillSwitchRequest& req);

  void OnBatchResult(const fob::risk::v1::PostTradeUpdateRequest& req);

 private:
  bool is_halted_locked(const std::string& symbol) const;

  mutable std::mutex mu_;
  bool global_halt_{false};
  std::unordered_map<std::string, bool> instrument_halt_; // symbol->halt

  infra::RiskAlertsPublisher publisher_;
};

}  // namespace cex::risk::app
