// ============================================================================
// ledger_f06_positions_test.cpp — F-06 (T-F06-021/022/023) unit tests.
//
// Covers the position-transition + mark-to-market rules from
// SEQ-LEDGER-001 / SEQ-LEDGER-002:
//   U1  uPnL long:  (mark-entry)*qty
//   U2  uPnL short: (entry-mark)*qty
//   U3  uPnL flat / qty=0 -> 0
//   U4  increase: weighted avg_entry_price, realized unchanged
//   U5  reduce (partial): realized on closed volume, avg preserved
//   U6  close (full): side=flat, quantity=0
//   U7  flip through zero: realized only on closed leg, new leg at fill_price
//   U7b short increase + cover with realized
//   U8  scale invariant after a series of operations
// ============================================================================
#include <cassert>
#include <iostream>

#include "app/ledger_uc.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;

namespace {

fob::common::v1::EventMeta make_meta(const std::string& cid) {
  fob::common::v1::EventMeta m;
  m.set_event_id("t");
  m.set_source("ledger-test");
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
                                      int64_t qty_u, int32_t qty_s,
                                      int64_t px_u, int32_t px_s) {
  fob::matching::v1::FlowFill f;
  f.set_order_id(order_id);
  f.set_user_id(user);
  f.mutable_instrument()->set_symbol(symbol);
  f.mutable_instrument()->set_base("BTC");
  f.mutable_instrument()->set_quote("USDT");
  f.set_side(side);
  *f.mutable_executed_qty() = dec(qty_u, qty_s);
  *f.mutable_price() = dec(px_u, px_s);
  // notional = qty * price (scale 2)
  __int128 notional = static_cast<__int128>(qty_u) * px_u;
  // qty scale 8, price scale 2 -> notional scale 10; rescale to 2
  notional /= 100000000;  // drop qty scale -> price scale (2)
  *f.mutable_executed_notional() = dec(static_cast<int64_t>(notional), 2);
  return f;
}

void apply(LedgerUseCases& uc, const std::string& batch,
           const fob::matching::v1::FlowFill& fill) {
  fob::ledger::v1::ApplyBatchResultRequest req;
  *req.mutable_meta() = make_meta(batch);
  req.mutable_batch()->set_batch_id(batch);
  *req.mutable_batch()->add_fills() = fill;
  auto resp = uc.ApplyBatchResult(req);
  assert(resp.success());
}

// U4: weighted avg on increase, realized unchanged.
void test_increase_weighted_avg() {
  LedgerUseCases uc;
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            100000000, 8, 5000000, 2));  // 1 @ 50000
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            100000000, 8, 6000000, 2));  // 1 @ 60000
  auto p = uc.GetPosition("u", "BTC/USDT");
  assert(p.amount.units == 200000000 && p.amount.scale == 8);     // 2 BTC
  assert(p.avg_entry_price.units == 5500000 && p.avg_entry_price.scale == 2);  // 55000
  assert(Decimal::cmp(p.realised_pnl, Decimal::zero()) == 0);
  std::cout << "U4 increase weighted avg: PASSED\n";
}

// U5: partial reduce, realized on closed volume, avg preserved.
void test_partial_reduce() {
  LedgerUseCases uc;
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            100000000, 8, 5000000, 2));   // long 1 @ 50000
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                            40000000, 8, 5500000, 2));    // sell 0.4 @ 55000
  auto p = uc.GetPosition("u", "BTC/USDT");
  assert(p.amount.units == 60000000);                  // 0.6 BTC left
  assert(p.avg_entry_price.units == 5000000);          // entry preserved
  // realized = (55000-50000)*0.4 = 2000
  assert(p.realised_pnl.units == 200000 && p.realised_pnl.scale == 2);
  std::cout << "U5 partial reduce: PASSED\n";
}

// U6: full close -> flat, qty 0, avg reset.
void test_full_close() {
  LedgerUseCases uc;
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            100000000, 8, 5000000, 2));
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                            100000000, 8, 5200000, 2));
  auto p = uc.GetPosition("u", "BTC/USDT");
  assert(Decimal::cmp(p.amount, Decimal::zero()) == 0);
  assert(Decimal::cmp(p.avg_entry_price, Decimal::zero()) == 0);
  // realized = (52000-50000)*1 = 2000
  assert(p.realised_pnl.units == 200000 && p.realised_pnl.scale == 2);
  // GetPositions: flat side
  fob::ledger::v1::GetPositionsRequest req; req.set_user_id("u");
  auto resp = uc.GetPositionsView(req, {});
  assert(resp.positions_size() == 1);
  assert(resp.positions(0).side() == "flat");
  std::cout << "U6 full close: PASSED\n";
}

// U7: flip through zero — realized only on closed leg, new leg at fill_price.
void test_flip() {
  LedgerUseCases uc;
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            100000000, 8, 5000000, 2));    // long 1 @ 50000
  // sell 1.5 @ 60000 -> close 1 (realized (60000-50000)*1 = 10000), open short 0.5 @ 60000
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                            150000000, 8, 6000000, 2));
  auto p = uc.GetPosition("u", "BTC/USDT");
  assert(p.amount.units == -50000000);                 // short 0.5 (signed)
  assert(p.avg_entry_price.units == 6000000);          // new leg @ 60000
  assert(p.realised_pnl.units == 1000000);             // 10000.00
  // GetPositions reports side=short, abs qty
  fob::ledger::v1::GetPositionsRequest req; req.set_user_id("u");
  auto resp = uc.GetPositionsView(req, {});
  assert(resp.positions(0).side() == "short");
  assert(resp.positions(0).quantity().units() == 50000000);
  std::cout << "U7 flip through zero: PASSED\n";
}

// U7b: open short, then cover (BUY) with realized for short.
void test_short_then_cover() {
  LedgerUseCases uc;
  // start flat, SELL 1 @ 50000 -> short 1
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                            100000000, 8, 5000000, 2));
  auto p1 = uc.GetPosition("u", "BTC/USDT");
  assert(p1.amount.units == -100000000);
  assert(p1.avg_entry_price.units == 5000000);
  // BUY 0.4 @ 45000 to cover -> realized short (entry-mark)*qty = (50000-45000)*0.4=2000
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            40000000, 8, 4500000, 2));
  auto p2 = uc.GetPosition("u", "BTC/USDT");
  assert(p2.amount.units == -60000000);            // short 0.6
  assert(p2.avg_entry_price.units == 5000000);     // preserved
  assert(p2.realised_pnl.units == 200000);         // 2000.00
  std::cout << "U7b short then cover: PASSED\n";
}

// U1/U2/U3: GetPositions mark-to-market.
void test_unrealized_mark_to_market() {
  LedgerUseCases uc;
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            100000000, 8, 5000000, 2));   // long 1 @ 50000
  fob::ledger::v1::GetPositionsRequest req; req.set_user_id("u");
  std::unordered_map<std::string, Decimal> marks;
  marks["BTC/USDT"] = Decimal{5500000, 2};  // mark 55000
  auto resp = uc.GetPositionsView(req, marks);
  assert(resp.positions_size() == 1);
  // U1 long: (55000-50000)*1 = 5000
  assert(resp.positions(0).unrealized_pnl().units() == 500000);
  assert(resp.positions(0).side() == "long");

  // U2 short: flip to short via heavy sell, then mark below entry = profit
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                            200000000, 8, 5000000, 2));   // short 1 @ 50000
  std::unordered_map<std::string, Decimal> marks2;
  marks2["BTC/USDT"] = Decimal{4800000, 2};  // mark 48000
  auto resp2 = uc.GetPositionsView(req, marks2);
  // short: (50000-48000)*1 = 2000
  assert(resp2.positions(0).side() == "short");
  assert(resp2.positions(0).unrealized_pnl().units() == 200000);

  // U3: no mark price for flat — close fully and check uPnL 0
  std::cout << "U1/U2 unrealized mark-to-market: PASSED\n";
}

// U3 (explicit): flat / qty==0 position reports unrealized_pnl == 0 and does
// NOT consult mark prices. Full-close a long, then request positions WITH a
// non-trivial mark for that symbol — uPnL must still be 0 for the flat leg.
void test_unrealized_flat_is_zero() {
  LedgerUseCases uc;
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            100000000, 8, 5000000, 2));    // long 1 @ 50000
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                            100000000, 8, 5200000, 2));    // close fully -> flat
  fob::ledger::v1::GetPositionsRequest req; req.set_user_id("u");
  std::unordered_map<std::string, Decimal> marks;
  marks["BTC/USDT"] = Decimal{9999900, 2};  // wildly different mark, must be ignored
  auto resp = uc.GetPositionsView(req, marks);
  assert(resp.positions_size() == 1);
  assert(resp.positions(0).side() == "flat");
  // uPnL for a flat position is 0 regardless of mark price.
  assert(resp.positions(0).unrealized_pnl().units() == 0);
  std::cout << "U3 flat/qty=0 unrealized = 0: PASSED\n";
}

// U10 (portfolio / multi-leg aggregation): a user holding several open
// positions (legs) on different symbols. Verifies that GetPositionsView
// returns each leg with its own correct uPnL AND that the portfolio totals
// (Σ realized, Σ unrealized) aggregate correctly across legs.
void test_multileg_portfolio_aggregation() {
  LedgerUseCases uc;
  // Leg A — BTC long: BUY 2 @ 50000, then SELL 1 @ 55000 (partial reduce).
  //   realized_A = (55000-50000)*1 = 5000; remaining long 1 @ 50000.
  apply(uc, "ba1", make_fill("oa1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                             200000000, 8, 5000000, 2));
  apply(uc, "ba2", make_fill("oa2", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                             100000000, 8, 5500000, 2));
  // Leg B — ETH short: SELL 10 @ 3000, then BUY 4 @ 2800 (partial cover).
  //   realized_B = (3000-2800)*4 = 800; remaining short 6 @ 3000.
  apply(uc, "bb1", make_fill("ob1", "u", "ETH/USDT", fob::common::v1::SIDE_SELL,
                             1000000000, 8, 300000, 2));
  apply(uc, "bb2", make_fill("ob2", "u", "ETH/USDT", fob::common::v1::SIDE_BUY,
                             400000000, 8, 280000, 2));

  // Per-leg realized check (positions store cumulative realized PnL).
  auto pa = uc.GetPosition("u", "BTC/USDT");
  auto pb = uc.GetPosition("u", "ETH/USDT");
  assert(pa.realised_pnl.units == 500000);  // 5000.00
  assert(pb.realised_pnl.units == 80000);   // 800.00
  // Portfolio Σ realized = 5000 + 800 = 5800.
  Decimal realized_total = Decimal::add(pa.realised_pnl, pb.realised_pnl);
  assert(realized_total.units == 580000 && realized_total.scale == 2);

  // Mark-to-market both legs.
  //   BTC long 1 @ entry 50000, mark 57000 -> uPnL_A = (57000-50000)*1 = 7000.
  //   ETH short 6 @ entry 3000, mark 2900 -> uPnL_B = (3000-2900)*6 = 600.
  fob::ledger::v1::GetPositionsRequest req; req.set_user_id("u");
  std::unordered_map<std::string, Decimal> marks;
  marks["BTC/USDT"] = Decimal{5700000, 2};
  marks["ETH/USDT"] = Decimal{290000, 2};
  auto resp = uc.GetPositionsView(req, marks);
  assert(resp.positions_size() == 2);

  // Aggregate uPnL and realized across the returned legs (portfolio view).
  Decimal upnl_sum = Decimal::zero();
  Decimal rpnl_sum = Decimal::zero();
  std::unordered_map<std::string, int64_t> upnl_by_symbol;
  for (int i = 0; i < resp.positions_size(); ++i) {
    const auto& pos = resp.positions(i);
    Decimal u{pos.unrealized_pnl().units(), pos.unrealized_pnl().scale()};
    Decimal r{pos.realized_pnl().units(), pos.realized_pnl().scale()};
    upnl_sum = Decimal::add(upnl_sum, u);
    rpnl_sum = Decimal::add(rpnl_sum, r);
    upnl_by_symbol[pos.symbol()] = pos.unrealized_pnl().units();
  }
  // Per-leg uPnL.
  assert(upnl_by_symbol["BTC/USDT"] == 700000);  // 7000.00
  assert(upnl_by_symbol["ETH/USDT"] == 60000);   // 600.00
  // Portfolio Σ uPnL = 7000 + 600 = 7600.
  assert(upnl_sum.units == 760000 && upnl_sum.scale == 2);
  // Portfolio Σ realized = 5800 (matches per-position cumulative).
  assert(rpnl_sum.units == 580000);
  std::cout << "U10 multi-leg portfolio aggregation: PASSED "
            << "(Σ uPnL=7600.00, Σ rPnL=5800.00)\n";
}

// U8: scale invariant — after a series of mixed fills the stored scales stay
// canonical (qty scale 8, price/pnl scale 2). Guards scale-inflation.
void test_scale_invariant() {
  LedgerUseCases uc;
  apply(uc, "b1", make_fill("o1", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            33333333, 8, 4999900, 2));
  apply(uc, "b2", make_fill("o2", "u", "BTC/USDT", fob::common::v1::SIDE_BUY,
                            77777777, 8, 5123400, 2));
  apply(uc, "b3", make_fill("o3", "u", "BTC/USDT", fob::common::v1::SIDE_SELL,
                            55555555, 8, 5200000, 2));
  auto p = uc.GetPosition("u", "BTC/USDT");
  assert(p.amount.scale == 8);
  assert(p.avg_entry_price.scale == 2);
  assert(p.realised_pnl.scale == 2);
  std::cout << "U8 scale invariant: PASSED (amount.scale=" << p.amount.scale
            << " avg.scale=" << p.avg_entry_price.scale
            << " rpnl.scale=" << p.realised_pnl.scale << ")\n";
}

}  // namespace

int main() {
  test_increase_weighted_avg();
  test_partial_reduce();
  test_full_close();
  test_flip();
  test_short_then_cover();
  test_unrealized_mark_to_market();
  test_unrealized_flat_is_zero();
  test_multileg_portfolio_aggregation();
  test_scale_invariant();
  std::cout << "All F-06 position tests passed!\n";
  return 0;
}
