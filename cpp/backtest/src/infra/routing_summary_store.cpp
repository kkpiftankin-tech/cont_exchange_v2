#include "infra/routing_summary_store.hpp"

namespace cex::backtest::infra {

RoutingReplaySummaryStore::RoutingReplaySummaryStore(
    app::IReplaySummaryStore* delegate,
    EphemeralSessionRegistry* registry)
    : delegate_(delegate), registry_(registry) {}

void RoutingReplaySummaryStore::SaveSummary(const app::ReplaySummary& summary) {
  // F15-PERSIST-1: ephemeral sessions produce no PG rows.
  if (registry_->IsEphemeral(summary.session_id)) return;
  if (delegate_ == nullptr) return;
  delegate_->SaveSummary(summary);
}

}  // namespace cex::backtest::infra
