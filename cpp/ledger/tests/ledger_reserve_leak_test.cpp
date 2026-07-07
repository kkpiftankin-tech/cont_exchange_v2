// ============================================================================
// ledger_reserve_leak_test.cpp — F-06 (T-F06-023) buy-reserve-leak fix.
//
// A BUY order reserves quote at price_high but fills cheaper. On the terminal
// FILLED order_update, the leftover (reserved - executed_notional) must be
// released back to available — reserved must NOT leak.
// ============================================================================
#include <cassert>
#include <iostream>

#include "app/ledger_uc.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;

namespace {

fob::common::v1::EventMeta meta(const std::string& c) {
  fob::common::v1::EventMeta m; m.set_correlation_id(c); m.set_source("t"); return m;
}
fob::common::v1::Decimal dec(int64_t u, int32_t s) {
  fob::common::v1::Decimal d; d.set_units(u); d.set_scale(s); return d;
}

void test_buy_reserve_no_leak() {
  LedgerUseCases uc;  // demo seed: 10000 USDT available
  // Reserve 5050 USDT (e.g. 0.1 BTC @ price_high 50500) for order o1.
  fob::ledger::v1::ReserveFundsRequest rr;
  *rr.mutable_meta() = meta("r1");
  rr.set_reservation_id("o1");
  rr.set_user_id("demo-user");
  rr.set_order_id("o1");
  rr.set_currency("USDT");
  *rr.mutable_amount() = dec(505000, 2);  // 5050.00
  auto rresp = uc.ReserveFunds(rr);
  assert(rresp.success());

  // Now a BUY fill executes 0.1 BTC @ 50000 = 5000 notional (cheaper than reserve).
  fob::ledger::v1::ApplyBatchResultRequest br;
  *br.mutable_meta() = meta("b1");
  br.mutable_batch()->set_batch_id("b1");
  auto* fill = br.mutable_batch()->add_fills();
  fill->set_order_id("o1");
  fill->set_user_id("demo-user");
  fill->mutable_instrument()->set_symbol("BTC/USDT");
  fill->mutable_instrument()->set_base("BTC");
  fill->mutable_instrument()->set_quote("USDT");
  fill->set_side(fob::common::v1::SIDE_BUY);
  *fill->mutable_executed_qty() = dec(10000000, 8);   // 0.1 BTC
  *fill->mutable_price() = dec(5000000, 2);           // 50000
  *fill->mutable_executed_notional() = dec(500000, 2);  // 5000.00
  // Terminal FILLED order_update triggers leftover release.
  auto* upd = br.mutable_batch()->add_order_updates();
  upd->set_order_id("o1");
  upd->set_status(fob::common::v1::ORDER_STATUS_FILLED);

  auto bresp = uc.ApplyBatchResult(br);
  assert(bresp.success());

  // Check USDT balance: started 10000, reserved 5050 -> avail 4950.
  // After fill: reserved -= 5000 notional -> reserved 50 leftover.
  // Terminal release: reserved 50 -> available, reserved = 0.
  // Final available = 4950 + 50 = 5000; reserved = 0.
  fob::ledger::v1::GetBalancesRequest gb;
  *gb.mutable_meta() = meta("g1");
  gb.set_user_id("demo-user");
  auto gresp = uc.GetBalances(gb);
  Decimal usdt_avail{0, 2}, usdt_reserved{0, 2};
  for (const auto& b : gresp.balances()) {
    if (b.currency() == "USDT") {
      usdt_avail = Decimal::from_proto(b.available());
      usdt_reserved = Decimal::from_proto(b.reserved());
    }
  }
  std::cout << "USDT available=" << usdt_avail.to_string()
            << " reserved=" << usdt_reserved.to_string() << "\n";
  // reserved must be 0 (no leak).
  assert(Decimal::cmp(usdt_reserved, Decimal::zero()) == 0);
  // available must be exactly 5000.00 (4950 + 50 released).
  assert(Decimal::cmp(usdt_avail, Decimal{500000, 2}) == 0);
  std::cout << "buy-reserve-leak fix: PASSED\n";
}

}  // namespace

int main() {
  test_buy_reserve_no_leak();
  std::cout << "All reserve-leak tests passed!\n";
  return 0;
}
