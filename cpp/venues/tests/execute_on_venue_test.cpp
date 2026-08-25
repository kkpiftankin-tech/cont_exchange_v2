// F-12 DoD-9 — Venue Execution Adapter unit suite (U1..U10).
//
// ExecuteOnVenue is a thin adapter: it builds the child intent (symbol
// resolution, strategy/tif defaults, client_order_id fallback), handles
// reconnect, forwards SendOrder to the VenueAdapter, and maps the
// returned VenueOrderResult into an ExecutionReport. The behaviour-rich
// outcomes (FILLED / PARTIALLY_FILLED / REJECTED / fee / slippage) are
// produced by the adapter and *mapped* here, so the U-cases that live
// at this layer are the mapping + child-intent-shaping ones. The rest
// live in other components and are covered by their own suites:
//
//   U1  Happy path FILLED              -> here (test_u1_*)
//   U2  Partial fill mapping            -> here (test_u2_*)  [retry: venues_loop / planning, out of layer]
//   U3  Overfill guard                  -> here (test_u3_*)  documents NO-TRIM passthrough; trimming is an unimplemented gap (status enum EXECUTION_REPORT_STATUS_OVERFILL_GUARD exists in PG mapping only)
//   U4  Rejection mapping               -> here (test_u4_*)  [fallback to next venue: matching planner, out of layer]
//   U5  Timeout status passthrough      -> here (test_u5_*)  [timeout detection: adapter-internal, out of layer]
//   U6  urgency->orderType mapping      -> matching execution_intent_builder (config-driven). strategy/tif DEFAULTS are here (test_u6_*).
//   U7  Reconciliation gap              -> ledger / venues_loop watchdog (out of layer; cross-ref note below)
//   U8  clientOrderId idempotency       -> here (test_u8_*)  fallback intent_id; true dedup is PG ON CONFLICT (postgres_child_order_repository)
//   U9  Multi-venue split               -> matching execution_planner (DoD-2, cpp/matching/tests/app/execution_planner_test.cpp)
//   U10 Pre-hedge risk reject           -> risk PreHedgeCheck (DoD-3, verified via grpcurl; cpp/risk/src/app/risk_uc.cpp)
//
// Plus supporting adapter-layer tests: symbol mapping, reconnect,
// missing-adapter, fee/slippage enrichment.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/execute_on_venue.hpp"

namespace {

using cex::venues::app::ExecuteOnVenue;
using cex::venues::domain::VenueAdapter;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueHeartbeat;
using cex::venues::domain::VenueOrderResult;
using cex::venues::domain::VenueRawSnapshot;
using cex::venues::domain::VenueSnapshotRequest;
using cex::venues::domain::VenueSubscription;
using cex::venues::domain::VenueType;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

fob::common::v1::Instrument BtcUsdt() {
  fob::common::v1::Instrument instrument;
  instrument.set_symbol("BTC/USDT");
  instrument.set_base("BTC");
  instrument.set_quote("USDT");
  return instrument;
}

fob::execution::v1::ExecutionIntent LimitIntent() {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("intent-1");
  intent.set_venue("binance");
  *intent.mutable_instrument() = BtcUsdt();
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(2500);
  intent.mutable_target_qty()->set_scale(3);
  intent.mutable_limit_price()->set_units(10123);
  intent.mutable_limit_price()->set_scale(2);
  return intent;
}

fob::execution::v1::ExecutionIntent MarketIntent() {
  auto intent = LimitIntent();
  intent.clear_limit_price();
  intent.set_venue("uniswap_v3");
  return intent;
}

class FakeVenueAdapter final : public VenueAdapter {
 public:
  explicit FakeVenueAdapter(std::string venue_id, VenueType type)
      : venue_id_(std::move(venue_id)), type_(type) {
    result_.accepted = true;
    result_.status = fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
    result_.venue_order_id = "ORDER-1";
  }

  std::string VenueId() const override { return venue_id_; }
  VenueType Type() const override { return type_; }

  bool Connect() override { return true; }
  bool Subscribe(const std::vector<VenueSubscription>& subscriptions) override {
    (void)subscriptions;
    return true;
  }
  bool Reconnect() override {
    ++reconnect_calls_;
    if (reconnect_ok_) heartbeat_status_ = VenueConnectionStatus::kConnected;
    return reconnect_ok_;
  }
  VenueHeartbeat Heartbeat() override {
    VenueHeartbeat hb;
    hb.venue_id = venue_id_;
    hb.venue_type = type_;
    hb.status = heartbeat_status_;
    return hb;
  }
  std::optional<VenueRawSnapshot> RequestSnapshot(
      const VenueSnapshotRequest& request) override {
    (void)request;
    return std::nullopt;
  }
  VenueOrderResult SendOrder(
      const fob::execution::v1::ExecutionIntent& intent) override {
    send_called_ = true;
    last_intent_ = intent;
    return result_;
  }
  bool ApplyRuntimeConfig(const cex::venues::domain::VenueAdapterRuntimeConfig&) override {
    return true;
  }

  void set_status(const VenueConnectionStatus status) { heartbeat_status_ = status; }
  void set_reconnect_ok(const bool value) { reconnect_ok_ = value; }
  void set_result(const VenueOrderResult& result) { result_ = result; }
  const fob::execution::v1::ExecutionIntent& last_intent() const { return last_intent_; }
  bool send_called() const { return send_called_; }
  int reconnect_calls() const { return reconnect_calls_; }

 private:
  std::string venue_id_;
  VenueType type_{VenueType::kCex};
  VenueConnectionStatus heartbeat_status_{VenueConnectionStatus::kConnected};
  bool reconnect_ok_{true};
  bool send_called_{false};
  int reconnect_calls_{0};
  VenueOrderResult result_;
  fob::execution::v1::ExecutionIntent last_intent_;
};

bool test_cex_mapping_and_defaults() {
  setenv("VENUE_SYMBOL_MAP", "binance|BTC/USDT=XBTUSDT", 1);
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  unsetenv("VENUE_SYMBOL_MAP");

  if (!Check(adapter.send_called(), "SendOrder must be called")) return false;
  if (!Check(adapter.last_intent().venue_symbol() == "XBTUSDT",
             "venue_symbol must come from map")) {
    return false;
  }
  if (!Check(adapter.last_intent().client_order_id() == "intent-1",
             "client_order_id must fallback to intent_id")) {
    return false;
  }
  if (!Check(adapter.last_intent().strategy() ==
                 fob::execution::v1::EXEC_STRATEGY_LIMIT,
             "limit order must default strategy=LIMIT")) {
    return false;
  }
  if (!Check(adapter.last_intent().tif() == fob::common::v1::TIF_GTC,
             "cex limit order must default tif=GTC")) {
    return false;
  }
  if (!Check(rep.venue_symbol() == "XBTUSDT",
             "report must keep normalized venue_symbol")) {
    return false;
  }

  return true;
}

bool test_dex_defaults_to_ioc() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("uniswap_v3", VenueType::kDex);

  const auto rep = uc.Run(MarketIntent(), &adapter);
  if (!Check(adapter.send_called(), "SendOrder must be called")) return false;
  if (!Check(adapter.last_intent().venue_symbol() == "BTCUSDT",
             "venue_symbol must fallback from instrument")) {
    return false;
  }
  if (!Check(adapter.last_intent().strategy() ==
                 fob::execution::v1::EXEC_STRATEGY_MARKET,
             "market order must default strategy=MARKET")) {
    return false;
  }
  if (!Check(adapter.last_intent().tif() == fob::common::v1::TIF_IOC,
             "dex order must default tif=IOC")) {
    return false;
  }
  if (!Check(rep.venue() == "uniswap_v3", "report venue must come from adapter")) {
    return false;
  }

  return true;
}

bool test_reconnect_before_send() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  adapter.set_status(VenueConnectionStatus::kDisconnected);
  adapter.set_reconnect_ok(true);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  if (!Check(adapter.reconnect_calls() == 1, "Reconnect must be attempted once")) {
    return false;
  }
  if (!Check(adapter.send_called(), "SendOrder must be called after reconnect")) {
    return false;
  }
  if (!Check(rep.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "successful reconnect must still return fill")) {
    return false;
  }

  return true;
}

bool test_missing_adapter_rejected() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;

  const auto rep = uc.Run(LimitIntent(), nullptr);
  if (!Check(rep.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "missing adapter must reject")) {
    return false;
  }
  if (!Check(rep.has_error() && rep.error().code() == "VENUE_NOT_FOUND",
             "missing adapter must set VENUE_NOT_FOUND")) {
    return false;
  }

  return true;
}

bool test_runtime_symbol_override_has_priority() {
  setenv("VENUE_SYMBOL_MAP", "binance|BTC/USDT=ENVPAIR", 1);
  ExecuteOnVenue uc;
  uc.SetVenueSymbolOverride("binance", "RUNTIMEPAIR");
  FakeVenueAdapter adapter("binance", VenueType::kCex);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  if (!Check(adapter.send_called(), "SendOrder must be called")) return false;
  if (!Check(adapter.last_intent().venue_symbol() == "RUNTIMEPAIR",
             "runtime symbol override must win over env map")) {
    return false;
  }
  if (!Check(rep.venue_symbol() == "RUNTIMEPAIR",
             "report must use runtime symbol override")) {
    return false;
  }

  uc.SetVenueSymbolOverride("binance", "");
  adapter = FakeVenueAdapter("binance", VenueType::kCex);
  const auto rep2 = uc.Run(LimitIntent(), &adapter);
  unsetenv("VENUE_SYMBOL_MAP");

  if (!Check(adapter.last_intent().venue_symbol() == "ENVPAIR",
             "clearing runtime override must fallback to env map")) {
    return false;
  }
  if (!Check(rep2.venue_symbol() == "ENVPAIR",
             "report must fallback to env map after clearing override")) {
    return false;
  }

  return true;
}

// Build a VenueOrderResult with a given status + quantities.
VenueOrderResult MakeResult(fob::execution::v1::ExecutionReportStatus status,
                            int64_t filled_units, int64_t remaining_units,
                            int64_t avg_price_units, int32_t price_scale = 2,
                            int32_t qty_scale = 3) {
  VenueOrderResult r;
  r.accepted = status != fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  r.status = status;
  r.venue_order_id = "VORDER-XYZ";
  r.filled_qty = cex::common::Decimal{filled_units, qty_scale};
  r.remaining_qty = cex::common::Decimal{remaining_units, qty_scale};
  r.average_price = cex::common::Decimal{avg_price_units, price_scale};
  return r;
}

// U1 — Happy path FILLED. Adapter returns full fill; report must mirror
// status, filled_qty, average_price, venue_order_id.
bool test_u1_happy_path_filled() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  // target 2.500 (units 2500 scale 3). Full fill at 101.23.
  adapter.set_result(MakeResult(
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
      /*filled*/ 2500, /*remaining*/ 0, /*avg*/ 10123));

  const auto rep = uc.Run(LimitIntent(), &adapter);
  bool ok = true;
  ok = Check(rep.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "U1: report status FILLED") && ok;
  ok = Check(rep.filled_qty().units() == 2500 && rep.filled_qty().scale() == 3,
             "U1: filled_qty mapped") && ok;
  ok = Check(rep.remaining_qty().units() == 0, "U1: remaining_qty zero") && ok;
  ok = Check(rep.average_price().units() == 10123,
             "U1: average_price mapped") && ok;
  ok = Check(rep.venue_order_id() == "VORDER-XYZ",
             "U1: venue_order_id mapped") && ok;
  ok = Check(!rep.has_error(), "U1: no error on fill") && ok;
  return ok;
}

// U2 — Partial fill mapping. filled < target, remaining > 0, status
// PARTIALLY_FILLED. (Retry/top-up is venues_loop/planning concern.)
bool test_u2_partial_fill_mapping() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  adapter.set_result(MakeResult(
      fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
      /*filled*/ 1500, /*remaining*/ 1000, /*avg*/ 10120));

  const auto rep = uc.Run(LimitIntent(), &adapter);
  bool ok = true;
  ok = Check(rep.status() ==
                 fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
             "U2: status PARTIALLY_FILLED") && ok;
  ok = Check(rep.filled_qty().units() == 1500, "U2: partial filled_qty") && ok;
  ok = Check(rep.remaining_qty().units() == 1000,
             "U2: remaining_qty > 0") && ok;
  return ok;
}

// U3 — Overfill guard. ExecuteOnVenue does NOT trim; it passes through
// whatever the adapter reports. This test documents the current
// (no-trim) behaviour so a future trimming change is a visible diff.
bool test_u3_overfill_passthrough_no_trim() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  // target 2.500 but adapter reports 2.600 filled (overfill).
  adapter.set_result(MakeResult(
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
      /*filled*/ 2600, /*remaining*/ 0, /*avg*/ 10123));

  const auto rep = uc.Run(LimitIntent(), &adapter);
  bool ok = true;
  // GAP DOCUMENTED: no trim to target (2500). If/when overfill guard
  // lands, this assertion flips and the test is updated.
  ok = Check(rep.filled_qty().units() == 2600,
             "U3: overfill passed through un-trimmed (documents gap)") && ok;
  return ok;
}

// U4 — Rejection mapping. Adapter rejects with a code; report must be
// REJECTED and carry the error. (Venue fallback is matching-planner.)
bool test_u4_rejection_mapping() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  VenueOrderResult r = MakeResult(
      fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED, 0, 2500, 0);
  r.error_code = "INSUFFICIENT_LIQUIDITY";
  r.error_message = "no book depth";
  adapter.set_result(r);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  bool ok = true;
  ok = Check(rep.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "U4: status REJECTED") && ok;
  ok = Check(rep.has_error() && rep.error().code() == "INSUFFICIENT_LIQUIDITY",
             "U4: error code propagated") && ok;
  ok = Check(rep.error().message() == "no book depth",
             "U4: error message propagated") && ok;
  return ok;
}

// U5 — Timeout/UNDERFILLED status passthrough. ExecuteOnVenue has no
// timeout logic of its own; whatever terminal status the adapter
// returns (here UNDERFILLED) must reach the report intact.
bool test_u5_underfilled_passthrough() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  adapter.set_result(MakeResult(
      fob::execution::v1::EXECUTION_REPORT_STATUS_UNDERFILLED,
      /*filled*/ 800, /*remaining*/ 1700, /*avg*/ 10125));

  const auto rep = uc.Run(LimitIntent(), &adapter);
  bool ok = true;
  ok = Check(rep.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_UNDERFILLED,
             "U5: UNDERFILLED status passthrough") && ok;
  ok = Check(rep.filled_qty().units() == 800, "U5: partial filled on timeout") && ok;
  return ok;
}

// U6 — strategy/tif DEFAULTS (the at-layer slice of urgency->orderType).
// Limit order with no explicit strategy/tif: strategy=LIMIT, tif=GTC on
// CEX. (urgency->strategy proper mapping lives in matching builder.)
bool test_u6_strategy_tif_defaults() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  uc.Run(LimitIntent(), &adapter);
  bool ok = true;
  ok = Check(adapter.last_intent().strategy() ==
                 fob::execution::v1::EXEC_STRATEGY_LIMIT,
             "U6: limit -> strategy LIMIT") && ok;
  ok = Check(adapter.last_intent().tif() == fob::common::v1::TIF_GTC,
             "U6: cex limit -> tif GTC") && ok;
  return ok;
}

// U8 — clientOrderId idempotency fallback. Empty client_order_id must
// fall back to intent_id (the stable idempotency key). True dedup is
// the PG ON CONFLICT (hedge_flow_id, client_order_id) in
// postgres_child_order_repository.
bool test_u8_client_order_id_fallback() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  auto intent = LimitIntent();
  intent.clear_client_order_id();   // force fallback
  uc.Run(intent, &adapter);
  bool ok = true;
  ok = Check(adapter.last_intent().client_order_id() == "intent-1",
             "U8: client_order_id falls back to intent_id") && ok;

  // explicit client_order_id is preserved
  auto intent2 = LimitIntent();
  intent2.set_client_order_id("explicit-coid");
  FakeVenueAdapter adapter2("binance", VenueType::kCex);
  uc.Run(intent2, &adapter2);
  ok = Check(adapter2.last_intent().client_order_id() == "explicit-coid",
             "U8: explicit client_order_id preserved") && ok;
  return ok;
}

// Fee / slippage enrichment mapping (VenueSim path): result.fee +
// fee_currency + slippage_bps must surface on the report.
bool test_fee_slippage_enrichment() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  VenueOrderResult r = MakeResult(
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED, 2500, 0, 10123);
  r.fee = cex::common::Decimal{253, 4};       // 0.0253
  r.fee_currency = "USDT";
  r.fee_rate = cex::common::Decimal{10, 4};   // 0.0010 (10 bps)
  r.slippage_bps = 7;
  adapter.set_result(r);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  bool ok = true;
  ok = Check(rep.slippage_bps() == 7, "fee/slip: slippage_bps mapped") && ok;
  ok = Check(rep.has_fee_total(), "fee/slip: fee_total present") && ok;
  ok = Check(rep.fee_total().cost().currency() == "USDT",
             "fee/slip: fee currency mapped") && ok;
  ok = Check(rep.fee_total().cost().amount().units() == 253,
             "fee/slip: fee amount mapped") && ok;
  return ok;
}

}  // namespace

int main() {
  bool all_passed = true;

  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  // adapter-layer support tests
  run("test_cex_mapping_and_defaults", test_cex_mapping_and_defaults);
  run("test_dex_defaults_to_ioc", test_dex_defaults_to_ioc);
  run("test_reconnect_before_send", test_reconnect_before_send);
  run("test_missing_adapter_rejected", test_missing_adapter_rejected);
  run("test_runtime_symbol_override_has_priority",
      test_runtime_symbol_override_has_priority);

  // F-12 DoD-9 U-suite (at-layer cases)
  run("U1_happy_path_filled", test_u1_happy_path_filled);
  run("U2_partial_fill_mapping", test_u2_partial_fill_mapping);
  run("U3_overfill_passthrough_no_trim", test_u3_overfill_passthrough_no_trim);
  run("U4_rejection_mapping", test_u4_rejection_mapping);
  run("U5_underfilled_passthrough", test_u5_underfilled_passthrough);
  run("U6_strategy_tif_defaults", test_u6_strategy_tif_defaults);
  run("U8_client_order_id_fallback", test_u8_client_order_id_fallback);
  run("fee_slippage_enrichment", test_fee_slippage_enrichment);

  if (all_passed) {
    std::cout << "[OK] execute_on_venue_test passed (13 tests; "
                 "U1/U2/U3/U4/U5/U6/U8 + 6 support). "
                 "U7 reconciliation -> ledger watchdog; "
                 "U9 multi-venue -> matching execution_planner_test; "
                 "U10 pre-hedge -> risk PreHedgeCheck."
              << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
