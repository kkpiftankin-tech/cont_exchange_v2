#pragma once

#include "app/replay_orchestration_ports.hpp"
#include "infra/ephemeral_session_registry.hpp"

namespace cex::backtest::infra {

// Implements IReplaySummaryStore with ephemeral routing.
//
//   ephemeral session  →  SaveSummary is a no-op (F15-PERSIST-1: nothing
//                         written to PostgreSQL replay_summaries)
//   persistent session →  delegate to the wrapped store
class RoutingReplaySummaryStore final : public app::IReplaySummaryStore {
 public:
  // delegate may be nullptr when PG is unavailable.
  explicit RoutingReplaySummaryStore(app::IReplaySummaryStore* delegate,
                                     EphemeralSessionRegistry* registry);

  void SaveSummary(const app::ReplaySummary& summary) override;

 private:
  app::IReplaySummaryStore* delegate_;
  EphemeralSessionRegistry* registry_;
};

}  // namespace cex::backtest::infra
