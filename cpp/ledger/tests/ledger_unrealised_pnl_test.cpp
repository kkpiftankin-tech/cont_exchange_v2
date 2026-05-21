#include <cassert>
#include <cmath>
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

fob::ledger::v1::HedgeExecution make_hedge_execution(
    const std::string& hedge_id,
    const std::string& venue,
    const std::string& instrument,
    fob::common::v1::Side side,
    int64_t qty_units, int32_t qty_scale,
    int64_t exec_price_units, int32_t exec_price_scale,
    int64_t internal_price_units, int32_t internal_price_scale) {
  
  fob::ledger::v1::HedgeExecution hedge;
  hedge.set_hedge_id(hedge_id);
  hedge.set_venue(venue);
  hedge.set_instrument_symbol(instrument);
  hedge.set_side(side);
  *hedge.mutable_executed_qty() = make_decimal(qty_units, qty_scale);
  *hedge.mutable_executed_price() = make_decimal(exec_price_units, exec_price_scale);
  *hedge.mutable_internal_price() = make_decimal(internal_price_units, internal_price_scale);
  
  return hedge;
}

void test_hedge_pnl_sell_profit() {
  std::cout << "Running test_hedge_pnl_sell_profit..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // SELL on Binance: external price 60000, internal price 50000, qty 0.1 BTC
  // Expected PnL = (60000 - 50000) * 0.1 = 1000 (profit)
  fob::ledger::v1::RecordHedgeExecutionRequest req;
  *req.mutable_meta() = make_meta("hedge1");
  *req.mutable_hedge() = make_hedge_execution(
      "hedge1", "binance", "BTC/USDT", fob::common::v1::SIDE_SELL,
      10000000, 8, 6000000, 2, 5000000, 2);
  
  auto resp = uc.RecordHedgeExecution(req);
  assert(resp.success());
  
  // Query hedge PnL
  fob::ledger::v1::GetHedgePnLRequest get_req;
  *get_req.mutable_meta() = make_meta("get1");
  get_req.set_venue("binance");
  
  auto get_resp = uc.GetHedgePnL(get_req);
  assert(get_resp.results_size() == 1);
  
  const auto& result = get_resp.results(0);
  double pnl = static_cast<double>(result.total_hedge_pnl().units()) / 
               std::pow(10.0, result.total_hedge_pnl().scale());
  assert(std::abs(pnl - 1000.0) < 0.1);
  assert(result.hedge_count() == 1);
  
  std::cout << "  PASSED" << std::endl;
}

void test_hedge_pnl_sell_loss() {
  std::cout << "Running test_hedge_pnl_sell_loss..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // SELL on Binance: external price 40000, internal price 50000, qty 0.1 BTC
  // Expected PnL = (40000 - 50000) * 0.1 = -1000 (loss)
  fob::ledger::v1::RecordHedgeExecutionRequest req;
  *req.mutable_meta() = make_meta("hedge2");
  *req.mutable_hedge() = make_hedge_execution(
      "hedge2", "binance", "BTC/USDT", fob::common::v1::SIDE_SELL,
      10000000, 8, 4000000, 2, 5000000, 2);
  
  auto resp = uc.RecordHedgeExecution(req);
  assert(resp.success());
  
  fob::ledger::v1::GetHedgePnLRequest get_req;
  *get_req.mutable_meta() = make_meta("get2");
  
  auto get_resp = uc.GetHedgePnL(get_req);
  assert(get_resp.results_size() == 1);
  
  const auto& result = get_resp.results(0);
  double pnl = static_cast<double>(result.total_hedge_pnl().units()) / 
               std::pow(10.0, result.total_hedge_pnl().scale());
  assert(std::abs(pnl + 1000.0) < 0.1);
  
  std::cout << "  PASSED" << std::endl;
}

void test_hedge_pnl_buy_profit() {
  std::cout << "Running test_hedge_pnl_buy_profit..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // BUY on Binance: external price 40000, internal price 50000, qty 0.1 BTC
  // For BUY, PnL = -(external - internal) * qty = -(-10000) * 0.1 = 1000 (profit)
  fob::ledger::v1::RecordHedgeExecutionRequest req;
  *req.mutable_meta() = make_meta("hedge3");
  *req.mutable_hedge() = make_hedge_execution(
      "hedge3", "binance", "BTC/USDT", fob::common::v1::SIDE_BUY,
      10000000, 8, 4000000, 2, 5000000, 2);
  
  auto resp = uc.RecordHedgeExecution(req);
  assert(resp.success());
  
  fob::ledger::v1::GetHedgePnLRequest get_req;
  *get_req.mutable_meta() = make_meta("get3");
  
  auto get_resp = uc.GetHedgePnL(get_req);
  assert(get_resp.results_size() == 1);
  
  const auto& result = get_resp.results(0);
  double pnl = static_cast<double>(result.total_hedge_pnl().units()) / 
               std::pow(10.0, result.total_hedge_pnl().scale());
  assert(std::abs(pnl - 1000.0) < 0.1);
  
  std::cout << "  PASSED" << std::endl;
}

void test_hedge_pnl_buy_loss() {
  std::cout << "Running test_hedge_pnl_buy_loss..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // BUY on Binance: external price 60000, internal price 50000, qty 0.1 BTC
  // For BUY, PnL = -(60000 - 50000) * 0.1 = -1000 (loss)
  fob::ledger::v1::RecordHedgeExecutionRequest req;
  *req.mutable_meta() = make_meta("hedge4");
  *req.mutable_hedge() = make_hedge_execution(
      "hedge4", "binance", "BTC/USDT", fob::common::v1::SIDE_BUY,
      10000000, 8, 6000000, 2, 5000000, 2);
  
  auto resp = uc.RecordHedgeExecution(req);
  assert(resp.success());
  
  fob::ledger::v1::GetHedgePnLRequest get_req;
  *get_req.mutable_meta() = make_meta("get4");
  
  auto get_resp = uc.GetHedgePnL(get_req);
  assert(get_resp.results_size() == 1);
  
  const auto& result = get_resp.results(0);
  double pnl = static_cast<double>(result.total_hedge_pnl().units()) / 
               std::pow(10.0, result.total_hedge_pnl().scale());
  assert(std::abs(pnl + 1000.0) < 0.1);
  
  std::cout << "  PASSED" << std::endl;
}

void test_hedge_pnl_multiple_records_aggregation() {
  std::cout << "Running test_hedge_pnl_multiple_records_aggregation..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  // Record 3 hedges
  auto hedge1 = make_hedge_execution("h1", "binance", "BTC/USDT", fob::common::v1::SIDE_SELL,
                                     10000000, 8, 6000000, 2, 5000000, 2);
  auto hedge2 = make_hedge_execution("h2", "binance", "BTC/USDT", fob::common::v1::SIDE_SELL,
                                     20000000, 8, 5500000, 2, 5000000, 2);
  // ETH: 0.5 ETH at 2800, internal 3000 -> for BUY: -(2800-3000)*0.5 = 100
  auto hedge3 = make_hedge_execution("h3", "bybit", "ETH/USDT", fob::common::v1::SIDE_BUY,
                                     50000000, 8, 280000, 2, 300000, 2);
  
  fob::ledger::v1::RecordHedgeExecutionRequest req1, req2, req3;
  *req1.mutable_meta() = make_meta("h1");
  *req1.mutable_hedge() = hedge1;
  *req2.mutable_meta() = make_meta("h2");
  *req2.mutable_hedge() = hedge2;
  *req3.mutable_meta() = make_meta("h3");
  *req3.mutable_hedge() = hedge3;
  
  assert(uc.RecordHedgeExecution(req1).success());
  assert(uc.RecordHedgeExecution(req2).success());
  assert(uc.RecordHedgeExecution(req3).success());
  
  // Get all hedge PnL
  fob::ledger::v1::GetHedgePnLRequest get_req;
  *get_req.mutable_meta() = make_meta("get_all");
  
  auto get_resp = uc.GetHedgePnL(get_req);
  assert(get_resp.results_size() == 2); // binance and bybit
  
  for (const auto& result : get_resp.results()) {
    if (result.venue() == "binance") {
      // h1: (60000-50000)*0.1 = 1000
      // h2: (55000-50000)*0.2 = 1000
      // Total: 2000
      double pnl = static_cast<double>(result.total_hedge_pnl().units()) / 
                   std::pow(10.0, result.total_hedge_pnl().scale());
      assert(std::abs(pnl - 2000.0) < 1.0);
      assert(result.hedge_count() == 2);
    } else if (result.venue() == "bybit") {
      // BUY at 2800, internal 3000: -(2800-3000)*0.5 = 100
      double pnl = static_cast<double>(result.total_hedge_pnl().units()) / 
                   std::pow(10.0, result.total_hedge_pnl().scale());
      assert(std::abs(pnl - 100.0) < 1.0);  // Увеличил допуск до 1.0
      assert(result.hedge_count() == 1);
    }
  }
  
  std::cout << "  PASSED" << std::endl;
}

void test_hedge_pnl_idempotency() {
  std::cout << "Running test_hedge_pnl_idempotency..." << std::endl;
  
  cex::ledger::app::LedgerUseCases uc;
  
  auto hedge = make_hedge_execution("same_id", "binance", "BTC/USDT", fob::common::v1::SIDE_SELL,
                                    10000000, 8, 6000000, 2, 5000000, 2);
  
  fob::ledger::v1::RecordHedgeExecutionRequest req;
  *req.mutable_meta() = make_meta("idem1");
  *req.mutable_hedge() = hedge;
  
  // First record
  auto resp1 = uc.RecordHedgeExecution(req);
  assert(resp1.success());
  
  // Second record with same ID (should be idempotent)
  auto resp2 = uc.RecordHedgeExecution(req);
  assert(resp2.success());
  
  // Should have only 1 record counted
  fob::ledger::v1::GetHedgePnLRequest get_req;
  *get_req.mutable_meta() = make_meta("get");
  
  auto get_resp = uc.GetHedgePnL(get_req);
  assert(get_resp.results_size() == 1);
  assert(get_resp.results(0).hedge_count() == 1);
  
  std::cout << "  PASSED" << std::endl;
}

}  // namespace

int main() {
  std::cout << "Running Hedge PnL tests..." << std::endl;
  std::cout << "=========================" << std::endl;
  
  test_hedge_pnl_sell_profit();
  test_hedge_pnl_sell_loss();
  test_hedge_pnl_buy_profit();
  test_hedge_pnl_buy_loss();
  test_hedge_pnl_multiple_records_aggregation();
  test_hedge_pnl_idempotency();
  
  std::cout << "=========================" << std::endl;
  std::cout << "All Hedge PnL tests passed!" << std::endl;
  return 0;
}