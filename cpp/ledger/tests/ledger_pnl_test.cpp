#include <cassert>
#include <iostream>

#include "app/ledger_uc.hpp"

using cex::common::Decimal;

namespace {

fob::common::v1::EventMeta make_meta(const std::string& correlation_id) {
  fob::common::v1::EventMeta meta;
  meta.set_event_id("test-event");
  meta.set_source("ledger-test");
  meta.set_correlation_id(correlation_id);
  return meta;
}

fob::common::v1::Decimal make_decimal(int64_t units, int32_t scale) {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

fob::matching::v1::FlowFill make_buy_fill(const std::string& order_id,
                                           const std::string& user_id,
                                           const std::string& symbol,
                                           int64_t qty_units, int32_t qty_scale,
                                           int64_t price_units, int32_t price_scale) {
  fob::matching::v1::FlowFill fill;
  fill.set_order_id(order_id);
  fill.set_user_id(user_id);
  fill.mutable_instrument()->set_symbol(symbol);
  fill.mutable_instrument()->set_base(symbol.substr(0, 3));
  fill.mutable_instrument()->set_quote(symbol.substr(4, 4));
  fill.set_side(fob::common::v1::SIDE_BUY);
  *fill.mutable_executed_qty() = make_decimal(qty_units, qty_scale);
  *fill.mutable_price() = make_decimal(price_units, price_scale);
  *fill.mutable_executed_notional() = make_decimal(qty_units * price_units / 100, price_scale);
  return fill;
}

fob::matching::v1::FlowFill make_sell_fill(const std::string& order_id,
                                           const std::string& user_id,
                                           const std::string& symbol,
                                           int64_t qty_units, int32_t qty_scale,
                                           int64_t price_units, int32_t price_scale) {
  auto fill = make_buy_fill(order_id, user_id, symbol, qty_units, qty_scale, price_units, price_scale);
  fill.set_side(fob::common::v1::SIDE_SELL);
  return fill;
}

void test_pnl_buy_then_sell_at_higher_price() {
  cex::ledger::app::LedgerUseCases uc;
  
  // Buy 1 BTC at 50000
  fob::ledger::v1::ApplyBatchResultRequest req1;
  *req1.mutable_meta() = make_meta("batch1");
  req1.mutable_batch()->set_batch_id("batch1");
  *req1.mutable_batch()->add_fills() = make_buy_fill("order1", "user1", "BTC/USDT", 100000000, 8, 5000000, 2);
  
  auto resp1 = uc.ApplyBatchResult(req1);
  assert(resp1.success());
  
  // Sell 0.5 BTC at 60000
  fob::ledger::v1::ApplyBatchResultRequest req2;
  *req2.mutable_meta() = make_meta("batch2");
  req2.mutable_batch()->set_batch_id("batch2");
  *req2.mutable_batch()->add_fills() = make_sell_fill("order2", "user1", "BTC/USDT", 50000000, 8, 6000000, 2);
  
  auto resp2 = uc.ApplyBatchResult(req2);
  assert(resp2.success());
  
  // Check position
  auto pos = uc.GetPosition("user1", "BTC/USDT");
  // Remaining: 0.5 BTC
  assert(pos.amount.units == 50000000);
  assert(pos.amount.scale == 8);
  // Avg price should still be 50000 (since we sold from the original lot)
  assert(pos.avg_entry_price.units == 5000000);
  assert(pos.avg_entry_price.scale == 2);
  // PnL = (60000 - 50000) * 0.5 = 5000 USDT
  assert(pos.realised_pnl.units == 500000);
  assert(pos.realised_pnl.scale == 2);
  
  std::cout << "test_pnl_buy_then_sell_at_higher_price: PASSED" << std::endl;
}

void test_pnl_buy_then_sell_at_lower_price() {
  cex::ledger::app::LedgerUseCases uc;
  
  // Buy 1 BTC at 50000
  fob::ledger::v1::ApplyBatchResultRequest req1;
  *req1.mutable_meta() = make_meta("batch1");
  req1.mutable_batch()->set_batch_id("batch1");
  *req1.mutable_batch()->add_fills() = make_buy_fill("order1", "user2", "BTC/USDT", 100000000, 8, 5000000, 2);
  
  auto resp1 = uc.ApplyBatchResult(req1);
  assert(resp1.success());
  
  // Sell 0.5 BTC at 40000 (loss)
  fob::ledger::v1::ApplyBatchResultRequest req2;
  *req2.mutable_meta() = make_meta("batch2");
  req2.mutable_batch()->set_batch_id("batch2");
  *req2.mutable_batch()->add_fills() = make_sell_fill("order2", "user2", "BTC/USDT", 50000000, 8, 4000000, 2);
  
  auto resp2 = uc.ApplyBatchResult(req2);
  assert(resp2.success());
  
  auto pos = uc.GetPosition("user2", "BTC/USDT");
  // PnL = (40000 - 50000) * 0.5 = -5000 USDT
  assert(pos.realised_pnl.units == -500000);
  assert(pos.realised_pnl.scale == 2);
  
  std::cout << "test_pnl_buy_then_sell_at_lower_price: PASSED" << std::endl;
}

void test_pnl_multiple_buys_then_sell() {
  cex::ledger::app::LedgerUseCases uc;
  
  // Buy 1 BTC at 50000
  fob::ledger::v1::ApplyBatchResultRequest req1;
  *req1.mutable_meta() = make_meta("batch1");
  req1.mutable_batch()->set_batch_id("batch1");
  *req1.mutable_batch()->add_fills() = make_buy_fill("order1", "user3", "BTC/USDT", 100000000, 8, 5000000, 2);
  
  auto resp1 = uc.ApplyBatchResult(req1);
  assert(resp1.success());
  
  // Buy another 1 BTC at 60000
  fob::ledger::v1::ApplyBatchResultRequest req2;
  *req2.mutable_meta() = make_meta("batch2");
  req2.mutable_batch()->set_batch_id("batch2");
  *req2.mutable_batch()->add_fills() = make_buy_fill("order2", "user3", "BTC/USDT", 100000000, 8, 6000000, 2);
  
  auto resp2 = uc.ApplyBatchResult(req2);
  assert(resp2.success());
  
  // Avg price should be (1*50000 + 1*60000)/2 = 55000
  auto pos_before = uc.GetPosition("user3", "BTC/USDT");
  assert(pos_before.avg_entry_price.units == 5500000);
  assert(pos_before.avg_entry_price.scale == 2);
  
  // Sell 1.5 BTC at 55000 (breakeven)
  fob::ledger::v1::ApplyBatchResultRequest req3;
  *req3.mutable_meta() = make_meta("batch3");
  req3.mutable_batch()->set_batch_id("batch3");
  *req3.mutable_batch()->add_fills() = make_sell_fill("order3", "user3", "BTC/USDT", 150000000, 8, 5500000, 2);
  
  auto resp3 = uc.ApplyBatchResult(req3);
  assert(resp3.success());
  
  auto pos_after = uc.GetPosition("user3", "BTC/USDT");
  // Remaining: 0.5 BTC
  assert(pos_after.amount.units == 50000000);
  // PnL should be ~0
  assert(std::abs(pos_after.realised_pnl.units) < 1000);
  
  std::cout << "test_pnl_multiple_buys_then_sell: PASSED" << std::endl;
}

}  // namespace

int main() {
  test_pnl_buy_then_sell_at_higher_price();
  test_pnl_buy_then_sell_at_lower_price();
  test_pnl_multiple_buys_then_sell();
  
  std::cout << "All PnL tests passed!" << std::endl;
  return 0;
}