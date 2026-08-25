// ============================================================================
// ledger_f06_integration_chain_test.cpp — F-06 (T-F06-061) in-process
// integration chain test. No Kafka, no PostgreSQL, no docker stack required.
//
// It wires the SAME application objects the live services use and drives the
// I2/I3/I4 chain in-process, so the cross-component contract (fill -> position
// -> GET /v1/positions payload -> margin snapshot) is verified deterministically
// here, while Testing/f06_it_e2e.sh proves the same chain over the real stack.
//
// Chain under test (matches SEQ-LEDGER-002 -> SEQ-LEDGER-001 -> SEQ-RISK-001):
//   1. ApplyBatchResult(batch with FlowFills)   [= ledger Kafka consumer apply]
//   2. GetPositionsView(req, marks)             [= source of GET /v1/positions]
//   3. ComputeMargin(positions from step 2)     [= risk buildRiskSnapshot]
//
// "I2": after applying a batch the position is updated (qty/side/avg/realized).
// "I3": the positions feed the margin calculator and produce a snapshot with
//        initial/maintenance margin and (when collateral is short) margin_call.
// "I4": the GetPositionsView payload carries exactly the fields the gateway
//        aggregates into GET /v1/positions (symbol/side/qty/avg/mark/uPnL/rPnL).
// ============================================================================
#include <cassert>
#include <iostream>
#include <vector>

#include "app/ledger_uc.hpp"
#include "domain/margin_calculator.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;
namespace rd = cex::risk::domain;

namespace {

fob::common::v1::EventMeta make_meta(const std::string& cid) {
  fob::common::v1::EventMeta m;
  m.set_event_id("it");
  m.set_source("f06-it-test");
  m.set_correlation_id(cid);
  return m;
}

fob::common::v1::Decimal dec(int64_t units, int32_t scale) {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

fob::matching::v1::FlowFill make_fill(const std::string& order_id,
                                      const std::string& user,
                                      const std::string& symbol,
                                      fob::common::v1::Side side,
                                      int64_t qty_u, int64_t px_u) {
  fob::matching::v1::FlowFill f;
  f.set_order_id(order_id);
  f.set_user_id(user);
  f.mutable_instrument()->set_symbol(symbol);
  f.mutable_instrument()->set_base("BTC");
  f.mutable_instrument()->set_quote("USDT");
  f.set_side(side);
  *f.mutable_executed_qty() = dec(qty_u, 8);
  *f.mutable_price() = dec(px_u, 2);
  __int128 notional = static_cast<__int128>(qty_u) * px_u / 100000000;
  *f.mutable_executed_notional() = dec(static_cast<int64_t>(notional), 2);
  return f;
}

}  // namespace

int main() {
  LedgerUseCases uc;
  const std::string user = "it-user";

  // --- Step 1 (I2): apply a batch with multiple fills in one ApplyBatchResult,
  // exactly as the ledger Kafka consumer does for a batch.outputs message.
  // This also covers I5 (several fills in one batch applied atomically).
  fob::ledger::v1::ApplyBatchResultRequest req;
  *req.mutable_meta() = make_meta("batch-it-1");
  req.mutable_batch()->set_batch_id("batch-it-1");
  // BTC: BUY 2 @ 50000, then SELL 0.5 @ 55000 (partial reduce, realized leg).
  *req.mutable_batch()->add_fills() =
      make_fill("o1", user, "BTC/USDT", fob::common::v1::SIDE_BUY, 200000000, 5000000);
  *req.mutable_batch()->add_fills() =
      make_fill("o2", user, "BTC/USDT", fob::common::v1::SIDE_SELL, 50000000, 5500000);
  // ETH: SELL 5 @ 3000 (open short leg).
  *req.mutable_batch()->add_fills() =
      make_fill("o3", user, "ETH/USDT", fob::common::v1::SIDE_SELL, 500000000, 300000);
  // All fills in one ApplyBatchResult are applied as a unit (I5: several fills
  // per batch). Batch-level idempotency (at-least-once safety) is enforced by a
  // wired IdempotencyRepositoryPort in the live service and is covered
  // separately by ledger_idempotency_test; this in-memory UC has no idempotency
  // repo wired, so we apply the batch exactly once here.
  auto apply_resp = uc.ApplyBatchResult(req);
  assert(apply_resp.success());

  auto btc = uc.GetPosition(user, "BTC/USDT");
  auto eth = uc.GetPosition(user, "ETH/USDT");
  // BTC long 1.5 @ 50000, realized = (55000-50000)*0.5 = 2500.
  assert(btc.amount.units == 150000000);
  assert(btc.avg_entry_price.units == 5000000);
  assert(btc.realised_pnl.units == 250000);
  // ETH short 5 @ 3000.
  assert(eth.amount.units == -500000000);
  assert(eth.avg_entry_price.units == 300000);
  std::cout << "I2/I5: batch with several fills applied as a unit -> positions updated: PASSED\n";

  // --- Step 2 (I4): GetPositionsView is the gateway's GET /v1/positions source.
  fob::ledger::v1::GetPositionsRequest gp;
  gp.set_user_id(user);
  std::unordered_map<std::string, Decimal> marks;
  marks["BTC/USDT"] = Decimal{5200000, 2};  // 52000
  marks["ETH/USDT"] = Decimal{280000, 2};   // 2800
  auto view = uc.GetPositionsView(gp, marks);
  assert(view.positions_size() == 2);

  // Build the margin-calculator input from the SAME view, as risk does.
  std::vector<rd::MarginPosition> mpos;
  Decimal upnl_total = Decimal::zero();
  for (int i = 0; i < view.positions_size(); ++i) {
    const auto& p = view.positions(i);
    // Every field the gateway serializes must be present and well-formed.
    assert(!p.symbol().empty());
    assert(p.side() == "long" || p.side() == "short" || p.side() == "flat");
    rd::MarginPosition m;
    m.symbol = p.symbol();
    m.quantity = Decimal{p.quantity().units(), p.quantity().scale()};
    m.mark_price = Decimal{p.mark_price().units(), p.mark_price().scale()};
    m.unrealized_pnl = Decimal{p.unrealized_pnl().units(), p.unrealized_pnl().scale()};
    upnl_total = Decimal::add(upnl_total, m.unrealized_pnl);
    mpos.push_back(m);
  }
  // BTC long uPnL = (52000-50000)*1.5 = 3000; ETH short uPnL = (3000-2800)*5 = 1000.
  // Σ uPnL = 4000.
  assert(upnl_total.units == 400000 && upnl_total.scale == 2);
  std::cout << "I4: GetPositionsView payload feeds gateway aggregation, Σ uPnL=4000.00: PASSED\n";

  // --- Step 3 (I3): risk buildRiskSnapshot consumes the positions and produces
  // a margin snapshot. notional = 1.5*52000 + 5*2800 = 78000 + 14000 = 92000.
  // initial (10%) = 9200; maintenance (5%) = 4600.
  rd::MarginRates rates;
  rates.initial_margin_rate = Decimal{1000, 4};      // 0.1
  rates.maintenance_margin_rate = Decimal{500, 4};   // 0.05
  // free_balance below maintenance to force a margin_call (I3 covers margin-call path).
  auto snap = rd::ComputeMargin(mpos, rates,
                                /*free_balance=*/Decimal{4000, 0},
                                /*reserved=*/Decimal{500, 0});
  assert(Decimal::cmp(snap.initial_margin, Decimal{9200, 0}) == 0);
  assert(Decimal::cmp(snap.maintenance_margin, Decimal{4600, 0}) == 0);
  // free_collateral = 4000 + Σ uPnL(4000) = 8000 -> > maintenance(4600): no call.
  assert(Decimal::cmp(snap.free_collateral, Decimal{8000, 0}) == 0);
  assert(!snap.margin_call);
  // Now drop collateral so free < maintenance -> margin_call.
  auto snap2 = rd::ComputeMargin(mpos, rates,
                                 /*free_balance=*/Decimal{0, 0},
                                 /*reserved=*/Decimal::zero());
  // free = 0 + 4000 = 4000 < maintenance 4600 -> margin_call.
  assert(snap2.margin_call);
  std::cout << "I3: positions -> ComputeMargin snapshot (initial=9200, maint=4600, margin_call path): PASSED\n";

  std::cout << "F-06 in-process integration chain (I2/I3/I4/I5): ALL PASSED\n";
  return 0;
}
