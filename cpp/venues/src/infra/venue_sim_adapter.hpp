#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/venue_adapter.hpp"
#include "infra/simulated_venue_adapter.hpp"

namespace cex::venues::infra {

// F12-BACKTEST-1 — VenueSim adapter.
//
// Drop-in replacement for the External Venues Connector
// (CexWsRestAdapter / DexAmmRpcAdapter) for backtest scenarios.
// Implements the same VenueAdapter contract so that the Venue Execution
// Adapter (ExecuteOnVenue), Execution Planning, Risk Manager and
// Settlement Ledger keep their business logic unchanged.
//
// All market-data lifecycle calls are delegated to a SimulatedVenueAdapter
// so historic LOB/snapshot replay still works. SendOrder is overridden and
// returns a deterministic VenueOrderResult driven by a policy that the
// backtest harness (and unit tests) configure per intent.
//
// F12-BACKTEST-2 will extend the policy with richer slippage/latency/fee
// models; this class deliberately keeps the policy small and explicit.
class VenueSimAdapter final : public domain::VenueAdapter {
 public:
  struct OrderPolicy {
    // 0..1. Fraction of intent.target_qty actually filled. Default = full fill.
    double fill_ratio{1.0};
    // Signed delta applied to limit_price.units (or mid.units when no limit
    // price is provided). Encodes deterministic slippage for backtest.
    int64_t slippage_units{0};
    // When set, overrides the status that VenueSim would otherwise derive
    // from fill_ratio.
    fob::execution::v1::ExecutionReportStatus forced_status{
        fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED};
    // When non-empty triggers a REJECTED result and propagates error fields.
    std::string reject_code;
    std::string reject_message;

    // F12-BACKTEST-2 — RawExecutionEvent enrichment.
    // When true VenueSim walks the latest cached snapshot's book to derive
    // filled_qty / average_price / slippage_bps / status. fill_ratio is
    // ignored when book walk is engaged.
    bool walk_book{false};
    // Taker commission rate expressed in basis points. 10 = 0.10%.
    int32_t taker_fee_bps{0};
    // Override of fee currency. When empty defaults to instrument.quote().
    std::string fee_currency;
    // Synthetic round-trip latency reported to the venue execution adapter
    // (informational; ExecuteOnVenue surfaces it on ExecutionReport meta).
    uint32_t latency_ms{0};
    // Reference price (in same scale as the LOB) used for slippage_bps.
    // When 0 VenueSim falls back to: limit_price → snapshot.mid_price → 0.
    int64_t reference_mid_units{0};
    // When the walked VWAP breaches intent.limit_price (BUY ask > limit,
    // SELL bid < limit) treat the order as CANCELLED with this reason.
    // When false the order is filled at the VWAP regardless of limit.
    bool enforce_limit_price{true};
  };

  explicit VenueSimAdapter(
      std::string venue_id,
      domain::VenueType venue_type = domain::VenueType::kCex,
      std::string session_id = "");

  // Domain identity.
  std::string VenueId() const override;
  domain::VenueType Type() const override;
  const std::string& SessionId() const;

  // Adapter lifecycle — delegated to the inner SimulatedVenueAdapter so that
  // backtest reuses the historic market-data simulator.
  bool Connect() override;
  bool Subscribe(const std::vector<domain::VenueSubscription>& subscriptions) override;
  bool Reconnect() override;
  domain::VenueHeartbeat Heartbeat() override;

  std::optional<domain::VenueRawSnapshot> RequestSnapshot(
      const domain::VenueSnapshotRequest& request) override;

  // Backtest order execution path. Replaces External Venues Connector.
  domain::VenueOrderResult SendOrder(
      const fob::execution::v1::ExecutionIntent& intent) override;

  bool ApplyRuntimeConfig(const domain::VenueAdapterRuntimeConfig& config) override;

  // Policy configuration (used by F12-BACKTEST-2 and tests).
  void SetDefaultOrderPolicy(const OrderPolicy& policy);
  // Per-intent override keyed by ExecutionIntent.intent_id() (falls back to
  // client_order_id when intent_id is empty).
  void SetOrderPolicyFor(const std::string& intent_or_client_order_id,
                         const OrderPolicy& policy);

  // Inject a historical / synthetic snapshot used by walk_book pricing
  // (F12-BACKTEST-2). Each RequestSnapshot() call also refreshes the cache.
  void SetLastSnapshot(const domain::VenueRawSnapshot& snapshot);
  std::optional<domain::VenueRawSnapshot> LastSnapshot() const;

  // Test/diagnostic accessors.
  uint64_t SentOrderCount() const;

 private:
  OrderPolicy ResolvePolicy(const fob::execution::v1::ExecutionIntent& intent) const;
  domain::VenueOrderResult BuildResult(
      const fob::execution::v1::ExecutionIntent& intent,
      const OrderPolicy& policy) const;

  std::string venue_id_;
  domain::VenueType venue_type_;
  std::string session_id_;
  std::unique_ptr<SimulatedVenueAdapter> inner_;

  mutable std::mutex mu_;
  OrderPolicy default_policy_{};
  std::unordered_map<std::string, OrderPolicy> per_intent_policies_;
  std::optional<domain::VenueRawSnapshot> last_snapshot_;
  uint64_t sent_orders_{0};
};

}  // namespace cex::venues::infra
