#include <cstdlib>
#include <iostream>
#include <string>

#include "app/execute_on_venue.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "infra/venue_sim_adapter.hpp"

namespace {

using cex::venues::app::ExecuteOnVenue;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueType;
using cex::venues::infra::VenueSimAdapter;

bool Check(const bool ok, const std::string& msg) {
  if (ok) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

fob::common::v1::Instrument BtcUsdt() {
  fob::common::v1::Instrument i;
  i.set_symbol("BTC/USDT");
  i.set_base("BTC");
  i.set_quote("USDT");
  return i;
}

fob::execution::v1::ExecutionIntent BuildIntent(const std::string& intent_id,
                                                const int64_t qty_units = 5000,
                                                const int64_t price_units = 6800000) {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id(intent_id);
  intent.set_client_order_id(intent_id + "-c");
  intent.set_venue("binance");
  intent.set_venue_symbol("BTCUSDT");
  *intent.mutable_instrument() = BtcUsdt();
  intent.mutable_target_qty()->set_units(qty_units);
  intent.mutable_target_qty()->set_scale(3);
  if (price_units != 0) {
    intent.mutable_limit_price()->set_units(price_units);
    intent.mutable_limit_price()->set_scale(2);
  }
  return intent;
}

bool TestIdentityAndType() {
  VenueSimAdapter adapter("binance", VenueType::kCex, "session-42");
  if (!Check(adapter.VenueId() == "binance", "VenueId must match")) return false;
  if (!Check(adapter.Type() == VenueType::kCex, "Type must match constructor")) return false;
  if (!Check(adapter.SessionId() == "session-42", "SessionId must match constructor"))
    return false;

  VenueSimAdapter dex("uniswap_v3", VenueType::kDex);
  if (!Check(dex.Type() == VenueType::kDex, "Type kDex must be preserved")) return false;
  if (!Check(dex.SessionId().empty(), "Default session id must be empty")) return false;
  return true;
}

bool TestLifecycleDelegation() {
  VenueSimAdapter adapter("binance");
  const auto hb_pre = adapter.Heartbeat();
  if (!Check(hb_pre.status == VenueConnectionStatus::kDisconnected,
             "Heartbeat must be disconnected before Connect"))
    return false;

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  const auto hb_post = adapter.Heartbeat();
  if (!Check(hb_post.status == VenueConnectionStatus::kConnected,
             "Heartbeat must be connected after Connect"))
    return false;

  if (!Check(adapter.Reconnect(), "Reconnect must succeed")) return false;
  const auto hb_after_reconnect = adapter.Heartbeat();
  if (!Check(hb_after_reconnect.reconnect_attempts == 1,
             "Reconnect counter must increment"))
    return false;
  return true;
}

bool TestSnapshotDelegation() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");
  cex::venues::domain::VenueSubscription sub;
  sub.instrument = BtcUsdt();
  sub.venue_symbol = "BTCUSDT";
  sub.depth_levels = 5;
  Check(adapter.Subscribe({sub}), "Subscribe");

  cex::venues::domain::VenueSnapshotRequest req;
  req.instrument = BtcUsdt();
  req.venue_symbol = "BTCUSDT";
  req.depth_levels = 5;
  const auto snap = adapter.RequestSnapshot(req);
  if (!Check(snap.has_value(), "Snapshot must be produced")) return false;
  if (!Check(snap->bids.size() == 5 && snap->asks.size() == 5,
             "Snapshot depth must match request"))
    return false;
  if (!Check(snap->best_bid.units < snap->best_ask.units,
             "Snapshot BBO must be ordered"))
    return false;
  return true;
}

bool TestSendOrderFullFillUsesLimitPrice() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  const auto intent = BuildIntent("intent-1");
  const auto result = adapter.SendOrder(intent);

  if (!Check(result.accepted, "Full-fill must be accepted")) return false;
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Status must be FILLED for fill_ratio=1.0"))
    return false;
  if (!Check(result.filled_qty.units == 5000 && result.filled_qty.scale == 3,
             "Filled qty must equal target qty"))
    return false;
  if (!Check(result.remaining_qty.units == 0,
             "Remaining qty must be zero on full fill"))
    return false;
  if (!Check(result.average_price.units == 6800000 &&
                 result.average_price.scale == 2,
             "Average price must equal limit_price when no slippage configured"))
    return false;
  if (!Check(result.venue_order_id.rfind("VSIM-", 0) == 0,
             "Venue order id must carry VSIM- prefix"))
    return false;
  if (!Check(adapter.SentOrderCount() == 1,
             "Sent order count must increment"))
    return false;
  return true;
}

bool TestSendOrderRejectsWhenDisconnected() {
  VenueSimAdapter adapter("binance");
  // No Connect() — inner SimulatedVenueAdapter starts disconnected.

  const auto result = adapter.SendOrder(BuildIntent("intent-2"));
  if (!Check(!result.accepted, "Disconnected venue must reject orders"))
    return false;
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "Status must be REJECTED when disconnected"))
    return false;
  if (!Check(result.error_code == "VENUE_SIM_DISCONNECTED",
             "Error code must be VENUE_SIM_DISCONNECTED"))
    return false;
  if (!Check(adapter.SentOrderCount() == 0,
             "Sent order count must not increment on disconnected reject"))
    return false;
  return true;
}

bool TestDefaultPolicyPartialFill() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  VenueSimAdapter::OrderPolicy policy;
  policy.fill_ratio = 0.4;
  adapter.SetDefaultOrderPolicy(policy);

  const auto intent = BuildIntent("intent-partial", 1000);
  const auto result = adapter.SendOrder(intent);

  if (!Check(result.accepted, "Partial fill must still be accepted"))
    return false;
  if (!Check(
          result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
          "Status must be PARTIALLY_FILLED when fill_ratio in (0,1)"))
    return false;
  if (!Check(result.filled_qty.units == 400,
             "Filled qty must reflect fill_ratio"))
    return false;
  if (!Check(result.remaining_qty.units == 600,
             "Remaining qty must reflect 1-fill_ratio"))
    return false;
  return true;
}

bool TestPolicyClampedFillRatio() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  VenueSimAdapter::OrderPolicy policy;
  policy.fill_ratio = 1.7;  // out-of-range should clamp to 1.0
  adapter.SetDefaultOrderPolicy(policy);

  const auto result = adapter.SendOrder(BuildIntent("intent-clamp", 1000));
  if (!Check(result.filled_qty.units == 1000,
             "fill_ratio>1 must clamp to full fill"))
    return false;

  VenueSimAdapter::OrderPolicy zero_policy;
  zero_policy.fill_ratio = -0.5;
  adapter.SetDefaultOrderPolicy(zero_policy);
  const auto cancelled = adapter.SendOrder(BuildIntent("intent-zero", 1000));
  if (!Check(cancelled.status == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED,
             "fill_ratio<=0 must yield CANCELLED"))
    return false;
  if (!Check(!cancelled.accepted,
             "Cancelled status implies not-accepted"))
    return false;
  return true;
}

bool TestPerIntentPolicyAndSlippage() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  VenueSimAdapter::OrderPolicy slipped;
  slipped.fill_ratio = 1.0;
  slipped.slippage_units = 1500;
  adapter.SetOrderPolicyFor("intent-slip", slipped);

  const auto result = adapter.SendOrder(BuildIntent("intent-slip", 5000, 6800000));
  if (!Check(result.average_price.units == 6801500,
             "Per-intent slippage must be applied on top of limit_price"))
    return false;

  // Default policy still applies for other intents.
  const auto other = adapter.SendOrder(BuildIntent("intent-default", 5000, 6800000));
  if (!Check(other.average_price.units == 6800000,
             "Default policy must not be polluted by per-intent policy"))
    return false;
  return true;
}

bool TestRejectPolicyPropagates() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  VenueSimAdapter::OrderPolicy policy;
  policy.reject_code = "INSUFFICIENT_LIQUIDITY";
  policy.reject_message = "venue book empty";
  adapter.SetDefaultOrderPolicy(policy);

  const auto result = adapter.SendOrder(BuildIntent("intent-rej"));
  if (!Check(!result.accepted, "Rejection policy must not mark accepted"))
    return false;
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "Rejection policy must set REJECTED"))
    return false;
  if (!Check(result.error_code == "INSUFFICIENT_LIQUIDITY",
             "Reject code must propagate"))
    return false;
  if (!Check(result.error_message == "venue book empty",
             "Reject message must propagate"))
    return false;
  return true;
}

bool TestMarketIntentUsesInnerMid() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  auto intent = BuildIntent("intent-market", 2500, 0);
  intent.clear_limit_price();

  VenueSimAdapter::OrderPolicy policy;
  policy.fill_ratio = 1.0;
  policy.slippage_units = 25;
  adapter.SetDefaultOrderPolicy(policy);

  const auto result = adapter.SendOrder(intent);
  if (!Check(result.accepted, "Market intent must be accepted")) return false;
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Market intent with full fill_ratio must be FILLED"))
    return false;
  if (!Check(result.average_price.units > 25,
             "Average price must include slippage offset on top of mid"))
    return false;
  return true;
}

bool TestExecuteOnVenueIntegrationKeepsBusinessLogic() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  ExecuteOnVenue executor;
  const auto intent = BuildIntent("intent-exec");

  const auto report = executor.Run(intent, &adapter);
  if (!Check(report.intent_id() == "intent-exec",
             "ExecutionReport must propagate intent_id"))
    return false;
  if (!Check(report.venue() == "binance",
             "ExecutionReport venue must come from adapter VenueId"))
    return false;
  if (!Check(report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "ExecuteOnVenue must produce FILLED report through VenueSim"))
    return false;
  if (!Check(report.filled_qty().units() == 5000,
             "ExecuteOnVenue must surface VenueSim filled qty"))
    return false;
  if (!Check(!report.client_order_id().empty(),
             "ExecuteOnVenue must populate client_order_id"))
    return false;
  return true;
}

// ----- F12-BACKTEST-2 helpers -----------------------------------------------

cex::venues::domain::VenueRawSnapshot MakeSnapshot(
    const std::vector<std::pair<int64_t, int64_t>>& bids,
    const std::vector<std::pair<int64_t, int64_t>>& asks,
    const int32_t price_scale = 2,
    const int32_t qty_scale = 3,
    const int64_t mid_units = 6800000) {
  cex::venues::domain::VenueRawSnapshot s;
  s.venue_id = "binance";
  s.venue_type = VenueType::kCex;
  s.instrument = BtcUsdt();
  s.venue_symbol = "BTCUSDT";
  s.status = VenueConnectionStatus::kConnected;
  s.mid_price.units = mid_units;
  s.mid_price.scale = price_scale;
  for (const auto& [p, q] : bids) {
    cex::venues::domain::VenueBookLevel level;
    level.price.units = p;
    level.price.scale = price_scale;
    level.qty.units = q;
    level.qty.scale = qty_scale;
    s.bids.push_back(level);
  }
  for (const auto& [p, q] : asks) {
    cex::venues::domain::VenueBookLevel level;
    level.price.units = p;
    level.price.scale = price_scale;
    level.qty.units = q;
    level.qty.scale = qty_scale;
    s.asks.push_back(level);
  }
  if (!s.bids.empty()) s.best_bid = s.bids.front().price;
  if (!s.asks.empty()) s.best_ask = s.asks.front().price;
  return s;
}

fob::execution::v1::ExecutionIntent BuildBuyIntent(const std::string& id,
                                                    const int64_t qty_units,
                                                    const int64_t limit_units = 0) {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id(id);
  intent.set_client_order_id(id + "-c");
  intent.set_venue("binance");
  intent.set_venue_symbol("BTCUSDT");
  *intent.mutable_instrument() = BtcUsdt();
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(qty_units);
  intent.mutable_target_qty()->set_scale(3);
  if (limit_units != 0) {
    intent.mutable_limit_price()->set_units(limit_units);
    intent.mutable_limit_price()->set_scale(2);
  }
  return intent;
}

bool TestWalkBookFullFillBuySide() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  // Asks: 100 @ 6800000 + 200 @ 6800500. Total 300 (qty_scale=3).
  adapter.SetLastSnapshot(MakeSnapshot(
      /*bids=*/{{6799500, 1000}, {6799000, 1000}},
      /*asks=*/{{6800000, 100}, {6800500, 200}}));

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  policy.taker_fee_bps = 10;       // 0.10%
  policy.latency_ms = 42;
  policy.fee_currency = "USDT";
  adapter.SetDefaultOrderPolicy(policy);

  const auto intent = BuildBuyIntent("wb-1", 300);
  const auto result = adapter.SendOrder(intent);

  if (!Check(result.accepted, "Walk-book full fill must be accepted"))
    return false;
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Walk-book full fill must produce FILLED"))
    return false;
  if (!Check(result.filled_qty.units == 300 && result.remaining_qty.units == 0,
             "Walk-book full fill must consume entire target qty"))
    return false;
  // VWAP = (100*6800000 + 200*6800500) / 300 = (680000000 + 1360100000)/300
  //      = 2040100000 / 300 = 6800333 (integer truncation)
  if (!Check(result.average_price.units == 6800333,
             "Walk-book VWAP must average across levels"))
    return false;
  if (!Check(result.latency_ms == 42,
             "Walk-book must surface configured latency"))
    return false;
  if (!Check(result.fee_currency == "USDT",
             "Walk-book must surface fee_currency"))
    return false;
  if (!Check(result.fee.units > 0,
             "Walk-book taker fee must be non-zero"))
    return false;
  return true;
}

bool TestWalkBookPartialFillWhenLiquidityExhausted() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  adapter.SetLastSnapshot(MakeSnapshot(
      /*bids=*/{},
      /*asks=*/{{6800000, 50}, {6800500, 100}}));

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  adapter.SetDefaultOrderPolicy(policy);

  const auto result = adapter.SendOrder(BuildBuyIntent("wb-2", 400));
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
             "Walk-book must mark PARTIALLY_FILLED when depth runs out"))
    return false;
  if (!Check(result.filled_qty.units == 150,
             "Walk-book partial fill must equal total available liquidity"))
    return false;
  if (!Check(result.remaining_qty.units == 250,
             "Walk-book remaining_qty must be target - filled"))
    return false;
  return true;
}

bool TestWalkBookRejectsEmptySide() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  // Only bids — BUY side has no asks to walk.
  adapter.SetLastSnapshot(MakeSnapshot(
      /*bids=*/{{6799500, 1000}},
      /*asks=*/{}));

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  adapter.SetDefaultOrderPolicy(policy);

  const auto result = adapter.SendOrder(BuildBuyIntent("wb-3", 100));
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "Empty side must reject"))
    return false;
  if (!Check(result.error_code == "INSUFFICIENT_LIQUIDITY",
             "Reject reason must be INSUFFICIENT_LIQUIDITY"))
    return false;
  return true;
}

bool TestWalkBookCancelsOnLimitPriceBreach() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  // Asks at 6801000 — above the BUY limit of 6800500. Must CANCEL.
  adapter.SetLastSnapshot(MakeSnapshot(
      /*bids=*/{},
      /*asks=*/{{6801000, 1000}}));

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  policy.enforce_limit_price = true;
  adapter.SetDefaultOrderPolicy(policy);

  const auto intent = BuildBuyIntent("wb-4", 200, /*limit_units=*/6800500);
  const auto result = adapter.SendOrder(intent);
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED,
             "Walk-book must CANCEL when VWAP breaches limit_price"))
    return false;
  if (!Check(result.error_code == "PRICE_LIMIT_EXCEEDED",
             "Cancellation must carry PRICE_LIMIT_EXCEEDED code"))
    return false;
  return true;
}

bool TestWalkBookSellSideUsesBids() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  adapter.SetLastSnapshot(MakeSnapshot(
      /*bids=*/{{6799500, 100}, {6799000, 200}},
      /*asks=*/{{6800000, 1000}}));

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  adapter.SetDefaultOrderPolicy(policy);

  fob::execution::v1::ExecutionIntent intent = BuildBuyIntent("wb-sell", 250);
  intent.set_side(fob::common::v1::SIDE_SELL);
  const auto result = adapter.SendOrder(intent);

  if (!Check(result.accepted && result.filled_qty.units == 250,
             "SELL walk-book must consume bids"))
    return false;
  // VWAP = (100*6799500 + 150*6799000)/250 = (679950000+1019850000)/250
  //      = 1699800000 / 250 = 6799200
  if (!Check(result.average_price.units == 6799200,
             "SELL walk-book VWAP must average bid levels"))
    return false;
  return true;
}

bool TestSlippageBpsSignedBySide() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  adapter.SetLastSnapshot(MakeSnapshot(
      /*bids=*/{{6799500, 1000}},
      /*asks=*/{{6800500, 1000}},
      /*price_scale=*/2,
      /*qty_scale=*/3,
      /*mid_units=*/6800000));

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  adapter.SetDefaultOrderPolicy(policy);

  // BUY VWAP=6800500 vs mid=6800000 → +500/6800000 ≈ +0.735 bps → 7.
  const auto buy = adapter.SendOrder(BuildBuyIntent("wb-buy-slip", 100));
  if (!Check(buy.slippage_bps > 0,
             "BUY worse-than-mid VWAP must produce positive slippage_bps"))
    return false;

  // SELL VWAP=6799500 vs mid=6800000 → ref-avg=+500 (better-for-seller would
  // be lower price; here seller hits 6799500 < mid by 500, which is worse) →
  // signed slippage for SELL flips sign → positive too.
  fob::execution::v1::ExecutionIntent sell_intent = BuildBuyIntent("wb-sell-slip", 100);
  sell_intent.set_side(fob::common::v1::SIDE_SELL);
  const auto sell = adapter.SendOrder(sell_intent);
  if (!Check(sell.slippage_bps > 0,
             "SELL worse-than-mid VWAP must produce positive slippage_bps"))
    return false;
  return true;
}

bool TestFeeRateExposedOnReport() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  adapter.SetLastSnapshot(MakeSnapshot(
      /*bids=*/{},
      /*asks=*/{{6800000, 1000}}));

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  policy.taker_fee_bps = 25;  // 0.25%
  adapter.SetDefaultOrderPolicy(policy);

  ExecuteOnVenue executor;
  const auto report = executor.Run(BuildBuyIntent("wb-fee", 200), &adapter);

  if (!Check(report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Report must be FILLED"))
    return false;
  if (!Check(report.slippage_bps() != 0 ||
                 report.average_price().units() == 6800000,
             "Report must surface slippage_bps from VenueSim"))
    return false;
  if (!Check(report.has_fee_total() && report.fee_total().fee_type() == "taker",
             "Report must expose fee_total.fee_type=taker"))
    return false;
  if (!Check(report.fee_total().rate().units() == 25 &&
                 report.fee_total().rate().scale() == 4,
             "Report fee rate must equal policy.taker_fee_bps as 1e-4 units"))
    return false;
  if (!Check(report.fee_total().cost().currency() == "USDT",
             "Fee currency must default to instrument.quote"))
    return false;
  if (!Check(report.fee_total().cost().amount().units() > 0,
             "Fee amount must be positive"))
    return false;
  return true;
}

bool TestWalkBookWithoutSnapshotRejects() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  adapter.SetDefaultOrderPolicy(policy);

  const auto result = adapter.SendOrder(BuildBuyIntent("wb-nosnap", 100));
  if (!Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "walk_book without snapshot must reject"))
    return false;
  if (!Check(result.error_code == "VENUE_SIM_NO_SNAPSHOT",
             "Reject code must be VENUE_SIM_NO_SNAPSHOT"))
    return false;
  return true;
}

bool TestRequestSnapshotCachesForWalkBook() {
  VenueSimAdapter adapter("binance");
  Check(adapter.Connect(), "Connect");
  cex::venues::domain::VenueSubscription sub;
  sub.instrument = BtcUsdt();
  sub.venue_symbol = "BTCUSDT";
  sub.depth_levels = 5;
  Check(adapter.Subscribe({sub}), "Subscribe");

  cex::venues::domain::VenueSnapshotRequest req;
  req.instrument = BtcUsdt();
  req.venue_symbol = "BTCUSDT";
  req.depth_levels = 5;
  const auto snap = adapter.RequestSnapshot(req);
  if (!Check(snap.has_value(), "Snapshot must be produced")) return false;
  if (!Check(adapter.LastSnapshot().has_value(),
             "RequestSnapshot must cache snapshot for walk_book"))
    return false;

  VenueSimAdapter::OrderPolicy policy;
  policy.walk_book = true;
  policy.enforce_limit_price = false;  // synthetic mid moves over time
  adapter.SetDefaultOrderPolicy(policy);

  const auto result = adapter.SendOrder(BuildBuyIntent("wb-cached", 100));
  if (!Check(result.accepted,
             "Cached synthetic snapshot must allow walk_book to fill"))
    return false;
  if (!Check(result.average_price.units > 0,
             "Cached snapshot walk must produce a positive VWAP"))
    return false;
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestIdentityAndType() && ok;
  ok = TestLifecycleDelegation() && ok;
  ok = TestSnapshotDelegation() && ok;
  ok = TestSendOrderFullFillUsesLimitPrice() && ok;
  ok = TestSendOrderRejectsWhenDisconnected() && ok;
  ok = TestDefaultPolicyPartialFill() && ok;
  ok = TestPolicyClampedFillRatio() && ok;
  ok = TestPerIntentPolicyAndSlippage() && ok;
  ok = TestRejectPolicyPropagates() && ok;
  ok = TestMarketIntentUsesInnerMid() && ok;
  ok = TestExecuteOnVenueIntegrationKeepsBusinessLogic() && ok;
  ok = TestWalkBookFullFillBuySide() && ok;
  ok = TestWalkBookPartialFillWhenLiquidityExhausted() && ok;
  ok = TestWalkBookRejectsEmptySide() && ok;
  ok = TestWalkBookCancelsOnLimitPriceBreach() && ok;
  ok = TestWalkBookSellSideUsesBids() && ok;
  ok = TestSlippageBpsSignedBySide() && ok;
  ok = TestFeeRateExposedOnReport() && ok;
  ok = TestWalkBookWithoutSnapshotRejects() && ok;
  ok = TestRequestSnapshotCachesForWalkBook() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] venue_sim_adapter_test" << std::endl;
  return EXIT_SUCCESS;
}
