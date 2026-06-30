// Unit tests for RoutingReplaySummaryStore (F15-PERSIST).
//
// Covers:
//   - ephemeral session ⇒ SaveSummary: delegate NOT called
//   - persistent session ⇒ SaveSummary: delegate called once

#include <iostream>
#include <string>

#include "app/replay_orchestration_ports.hpp"
#include "app/replay_session.hpp"
#include "infra/ephemeral_session_registry.hpp"
#include "infra/routing_summary_store.hpp"

namespace {

using cex::backtest::app::IReplaySummaryStore;
using cex::backtest::app::ReplaySummary;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::infra::EphemeralSessionRegistry;
using cex::backtest::infra::RoutingReplaySummaryStore;

bool Check(bool condition, const std::string& msg) {
  if (condition) return true;
  std::cerr << "[FAIL] " << msg << "\n";
  return false;
}

struct SpySummaryStore final : public IReplaySummaryStore {
  int save_calls{0};
  void SaveSummary(const ReplaySummary&) override { ++save_calls; }
};

ReplaySession MakeEphemeralSession(const std::string& id) {
  ReplaySession s;
  s.session_id = id;
  s.persist = false;
  s.status = ReplaySessionStatus::kPending;
  return s;
}

ReplaySummary MakeSummary(const std::string& session_id) {
  ReplaySummary sum;
  sum.session_id = session_id;
  return sum;
}

}  // namespace

int main() {
  int failures = 0;

  // ---- ephemeral: SaveSummary is no-op ----
  {
    SpySummaryStore spy;
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeralSession("eph-sum-1"));

    RoutingReplaySummaryStore store(&spy, &reg);
    store.SaveSummary(MakeSummary("eph-sum-1"));
    failures += Check(spy.save_calls == 0, "delegate not called for ephemeral SaveSummary");
  }

  // ---- persistent: SaveSummary delegates ----
  {
    SpySummaryStore spy;
    EphemeralSessionRegistry reg;
    // "pg-sum-1" is NOT in registry, so it is treated as persistent.

    RoutingReplaySummaryStore store(&spy, &reg);
    store.SaveSummary(MakeSummary("pg-sum-1"));
    failures += Check(spy.save_calls == 1, "delegate called once for persistent SaveSummary");
  }

  // ---- delegate=null, persistent: no crash ----
  {
    EphemeralSessionRegistry reg;
    RoutingReplaySummaryStore store(nullptr, &reg);
    store.SaveSummary(MakeSummary("pg-null-sum"));  // must not throw
    failures += Check(true, "no crash with null delegate for persistent");
  }

  if (failures == 0) {
    std::cout << "routing_summary_store_test: all tests passed\n";
    return 0;
  }
  std::cerr << "routing_summary_store_test: " << failures << " test(s) FAILED\n";
  return 1;
}
