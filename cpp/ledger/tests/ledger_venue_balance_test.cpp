#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

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

void test_venue_balance_update_and_get() {
  std::cout << "Running test_venue_balance_update_and_get..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // Update Binance USDT balance
  fob::ledger::v1::UpdateVenueBalanceRequest update_req;
  *update_req.mutable_meta() = make_meta("update1");
  update_req.set_venue("binance");
  update_req.set_currency("USDT");
  *update_req.mutable_total() = make_decimal(1000000, 2);  // 10000.00 USDT
  *update_req.mutable_reserved() = make_decimal(10000, 2);  // 100.00 USDT reserved
  
  auto update_resp = uc.UpdateVenueBalance(update_req);
  assert(update_resp.success());
  
  // Get venue balances
  fob::ledger::v1::GetVenueBalancesRequest get_req;
  *get_req.mutable_meta() = make_meta("get1");
  
  auto get_resp = uc.GetVenueBalances(get_req);
  
  assert(get_resp.balances_size() == 1);
  const auto& balance = get_resp.balances(0);
  assert(balance.venue() == "binance");
  assert(balance.currency() == "USDT");
  assert(balance.total().units() == 1000000);
  assert(balance.total().scale() == 2);
  assert(balance.reserved().units() == 10000);
  assert(balance.reserved().scale() == 2);
  assert(balance.available().units() == 990000);
  assert(balance.available().scale() == 2);
  
  std::cout << "  PASSED" << std::endl;
}

void test_venue_balance_multiple_venues() {
  std::cout << "Running test_venue_balance_multiple_venues..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // Update Binance balances
  fob::ledger::v1::UpdateVenueBalanceRequest update_binance;
  *update_binance.mutable_meta() = make_meta("update_binance");
  update_binance.set_venue("binance");
  update_binance.set_currency("USDT");
  *update_binance.mutable_total() = make_decimal(1000000, 2);
  *update_binance.mutable_reserved() = make_decimal(0, 2);
  uc.UpdateVenueBalance(update_binance);
  
  // Update Bybit balances
  fob::ledger::v1::UpdateVenueBalanceRequest update_bybit;
  *update_bybit.mutable_meta() = make_meta("update_bybit");
  update_bybit.set_venue("bybit");
  update_bybit.set_currency("USDT");
  *update_bybit.mutable_total() = make_decimal(500000, 2);
  *update_bybit.mutable_reserved() = make_decimal(0, 2);
  uc.UpdateVenueBalance(update_bybit);
  
  // Get all venue balances
  fob::ledger::v1::GetVenueBalancesRequest get_all;
  *get_all.mutable_meta() = make_meta("get_all");
  auto get_all_resp = uc.GetVenueBalances(get_all);
  assert(get_all_resp.balances_size() == 2);
  
  // Filter by venue
  fob::ledger::v1::GetVenueBalancesRequest get_binance;
  *get_binance.mutable_meta() = make_meta("get_binance");
  get_binance.set_venue("binance");
  auto get_binance_resp = uc.GetVenueBalances(get_binance);
  assert(get_binance_resp.balances_size() == 1);
  assert(get_binance_resp.balances(0).venue() == "binance");
  
  // Filter by currency
  fob::ledger::v1::GetVenueBalancesRequest get_usdt;
  *get_usdt.mutable_meta() = make_meta("get_usdt");
  get_usdt.set_currency("USDT");
  auto get_usdt_resp = uc.GetVenueBalances(get_usdt);
  assert(get_usdt_resp.balances_size() == 2);
  
  std::cout << "  PASSED" << std::endl;
}

void test_venue_balance_multiple_currencies() {
  std::cout << "Running test_venue_balance_multiple_currencies..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // Update Binance USDT
  fob::ledger::v1::UpdateVenueBalanceRequest update_usdt;
  *update_usdt.mutable_meta() = make_meta("update_usdt");
  update_usdt.set_venue("binance");
  update_usdt.set_currency("USDT");
  *update_usdt.mutable_total() = make_decimal(1000000, 2);
  *update_usdt.mutable_reserved() = make_decimal(0, 2);
  uc.UpdateVenueBalance(update_usdt);
  
  // Update Binance BTC
  fob::ledger::v1::UpdateVenueBalanceRequest update_btc;
  *update_btc.mutable_meta() = make_meta("update_btc");
  update_btc.set_venue("binance");
  update_btc.set_currency("BTC");
  *update_btc.mutable_total() = make_decimal(100000000, 8);
  *update_btc.mutable_reserved() = make_decimal(0, 8);
  uc.UpdateVenueBalance(update_btc);
  
  // Get all balances for binance
  fob::ledger::v1::GetVenueBalancesRequest get_req;
  *get_req.mutable_meta() = make_meta("get");
  get_req.set_venue("binance");
  auto get_resp = uc.GetVenueBalances(get_req);
  assert(get_resp.balances_size() == 2);
  
  // Verify both currencies present
  bool found_usdt = false;
  bool found_btc = false;
  for (const auto& b : get_resp.balances()) {
    if (b.currency() == "USDT") found_usdt = true;
    if (b.currency() == "BTC") found_btc = true;
  }
  assert(found_usdt);
  assert(found_btc);
  
  std::cout << "  PASSED" << std::endl;
}

void test_venue_balance_update_overwrites() {
  std::cout << "Running test_venue_balance_update_overwrites..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // First update
  fob::ledger::v1::UpdateVenueBalanceRequest update1;
  *update1.mutable_meta() = make_meta("update1");
  update1.set_venue("binance");
  update1.set_currency("USDT");
  *update1.mutable_total() = make_decimal(1000000, 2);
  *update1.mutable_reserved() = make_decimal(10000, 2);
  uc.UpdateVenueBalance(update1);
  
  // Second update (overwrite)
  fob::ledger::v1::UpdateVenueBalanceRequest update2;
  *update2.mutable_meta() = make_meta("update2");
  update2.set_venue("binance");
  update2.set_currency("USDT");
  *update2.mutable_total() = make_decimal(2000000, 2);
  *update2.mutable_reserved() = make_decimal(20000, 2);
  uc.UpdateVenueBalance(update2);
  
  // Verify overwritten
  fob::ledger::v1::GetVenueBalancesRequest get_req;
  *get_req.mutable_meta() = make_meta("get");
  get_req.set_venue("binance");
  get_req.set_currency("USDT");
  auto get_resp = uc.GetVenueBalances(get_req);
  
  assert(get_resp.balances_size() == 1);
  assert(get_resp.balances(0).total().units() == 2000000);
  assert(get_resp.balances(0).reserved().units() == 20000);
  
  std::cout << "  PASSED" << std::endl;
}

void test_venue_balance_invalid_input() {
  std::cout << "Running test_venue_balance_invalid_input..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // Empty venue
  fob::ledger::v1::UpdateVenueBalanceRequest req1;
  *req1.mutable_meta() = make_meta("req1");
  req1.set_venue("");
  req1.set_currency("USDT");
  *req1.mutable_total() = make_decimal(1000, 2);
  *req1.mutable_reserved() = make_decimal(0, 2);
  auto resp1 = uc.UpdateVenueBalance(req1);
  assert(!resp1.success());
  assert(resp1.error().code() == "INVALID_VENUE");
  
  // Empty currency
  fob::ledger::v1::UpdateVenueBalanceRequest req2;
  *req2.mutable_meta() = make_meta("req2");
  req2.set_venue("binance");
  req2.set_currency("");
  *req2.mutable_total() = make_decimal(1000, 2);
  *req2.mutable_reserved() = make_decimal(0, 2);
  auto resp2 = uc.UpdateVenueBalance(req2);
  assert(!resp2.success());
  assert(resp2.error().code() == "INVALID_CURRENCY");
  
  std::cout << "  PASSED" << std::endl;
}

}  // namespace

int main() {
  std::cout << "Running Venue Balance tests..." << std::endl;
  std::cout << "=============================" << std::endl;
  
  test_venue_balance_update_and_get();
  test_venue_balance_multiple_venues();
  test_venue_balance_multiple_currencies();
  test_venue_balance_update_overwrites();
  test_venue_balance_invalid_input();
  
  std::cout << "=============================" << std::endl;
  std::cout << "All Venue Balance tests passed!" << std::endl;
  return 0;
}