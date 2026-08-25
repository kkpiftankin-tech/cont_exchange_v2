#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app/metrics.hpp"
#include "app/backtest_uc.hpp"

namespace {

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Near(double a, double b, double eps = 1e-6) {
  return std::abs(a - b) < eps;
}

fob::matching::v1::BatchResult MakeBatch() {
  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-m-1");
  batch.mutable_timestamp()->set_seconds(1700000000);
  batch.mutable_timestamp()->set_nanos(0);

  // Clear price for BTC/USDT = 100.01
  (*batch.mutable_clear_prices())["BTC/USDT"].set_units(10001);
  (*batch.mutable_clear_prices())["BTC/USDT"].set_scale(2);

  batch.mutable_diagnostics()->set_residual_norm(0.00042);
  batch.mutable_diagnostics()->set_solve_time_ms(12);
  batch.mutable_diagnostics()->set_num_active_orders(2);

  // Fill 1: BUY 0.1 BTC @ 100.01 (= clearing price)
  auto* f1 = batch.add_fills();
  f1->set_order_id("order-1");
  f1->set_user_id("user-1");
  f1->mutable_instrument()->set_symbol("BTC/USDT");
  f1->mutable_instrument()->set_base("BTC");
  f1->mutable_instrument()->set_quote("USDT");
  f1->set_side(fob::common::v1::SIDE_BUY);
  f1->mutable_executed_qty()->set_units(100000);
  f1->mutable_executed_qty()->set_scale(6);    // 0.1
  f1->mutable_price()->set_units(10001);
  f1->mutable_price()->set_scale(2);           // 100.01
  f1->mutable_executed_notional()->set_units(10001000);
  f1->mutable_executed_notional()->set_scale(6); // 10.001
  f1->mutable_fee()->mutable_cost()->mutable_amount()->set_units(50);
  f1->mutable_fee()->mutable_cost()->mutable_amount()->set_scale(4); // 0.005
  f1->mutable_fee()->mutable_cost()->set_currency("USDT");
  f1->set_liquidity_source("internal");
  f1->mutable_provenance()->set_liquidity_source("internal");

  // Fill 2: SELL 0.05 BTC @ 100.03 (slightly above clearing price)
  auto* f2 = batch.add_fills();
  f2->set_order_id("order-2");
  f2->set_user_id("user-2");
  f2->mutable_instrument()->set_symbol("BTC/USDT");
  f2->mutable_instrument()->set_base("BTC");
  f2->mutable_instrument()->set_quote("USDT");
  f2->set_side(fob::common::v1::SIDE_SELL);
  f2->mutable_executed_qty()->set_units(50000);
  f2->mutable_executed_qty()->set_scale(6);    // 0.05
  f2->mutable_price()->set_units(10003);
  f2->mutable_price()->set_scale(2);           // 100.03
  f2->mutable_executed_notional()->set_units(5001500);
  f2->mutable_executed_notional()->set_scale(6); // 5.0015
  f2->mutable_fee()->mutable_cost()->mutable_amount()->set_units(25);
  f2->mutable_fee()->mutable_cost()->mutable_amount()->set_scale(4); // 0.0025
  f2->mutable_fee()->mutable_cost()->set_currency("USDT");
  f2->set_liquidity_source("cex_hedge");
  f2->mutable_provenance()->set_liquidity_source("cex_hedge");
  f2->mutable_provenance()->set_venue_id("binance");
  f2->mutable_provenance()->set_snapshot_id("snapshot-1");
  f2->mutable_provenance()->set_curve_id("curve-1");

  return batch;
}

bool test_fill_metrics_count() {
  auto batch = MakeBatch();
  auto fm = cex::backtest::app::MetricsCalculator::ComputeFillMetrics(batch);
  return Check(fm.size() == 2, "should produce 2 fill metrics");
}

bool test_fill_metrics_buy() {
  auto batch = MakeBatch();
  auto fm = cex::backtest::app::MetricsCalculator::ComputeFillMetrics(batch);
  const auto& buy = fm[0];

  if (!Check(buy.order_id == "order-1", "order_id")) return false;
  if (!Check(buy.side == "buy", "side")) return false;
  if (!Check(Near(buy.executed_qty, 0.1), "executed_qty")) return false;
  if (!Check(Near(buy.price, 100.01), "price")) return false;
  if (!Check(Near(buy.clear_price, 100.01), "clear_price")) return false;
  if (!Check(buy.liquidity_source == "internal", "buy liquidity_source")) return false;
  if (!Check(buy.venue_id.empty(), "buy venue_id")) return false;

  // IS: BUY at clearing price => is_bps = 0
  if (!Check(Near(buy.is_bps, 0.0, 0.01), "is_bps should be ~0 when price==clear_price")) {
    std::cerr << "  actual is_bps: " << buy.is_bps << std::endl;
    return false;
  }

  // PnL: BUY => -(notional + fee) = -(10.001 + 0.005) = -10.006
  if (!Check(Near(buy.fill_pnl, -10.006, 0.001), "fill_pnl for buy")) {
    std::cerr << "  actual fill_pnl: " << buy.fill_pnl << std::endl;
    return false;
  }

  return true;
}

bool test_fill_metrics_sell() {
  auto batch = MakeBatch();
  auto fm = cex::backtest::app::MetricsCalculator::ComputeFillMetrics(batch);
  const auto& sell = fm[1];

  if (!Check(sell.order_id == "order-2", "order_id")) return false;
  if (!Check(sell.side == "sell", "side")) return false;
  if (!Check(Near(sell.price, 100.03), "price")) return false;
  if (!Check(Near(sell.clear_price, 100.01), "clear_price")) return false;
  if (!Check(sell.liquidity_source == "cex_hedge", "sell liquidity_source")) return false;
  if (!Check(sell.venue_id == "binance", "sell venue_id")) return false;
  if (!Check(sell.snapshot_id == "snapshot-1", "sell snapshot_id")) return false;
  if (!Check(sell.curve_id == "curve-1", "sell curve_id")) return false;

  // IS for SELL: (clear_price / exec_price - 1) * 10000
  // = (100.01 / 100.03 - 1) * 10000 = -0.19994... bps (negative = better than clearing)
  if (!Check(sell.is_bps < 0.0, "is_bps should be negative (sold above clearing)")) {
    std::cerr << "  actual is_bps: " << sell.is_bps << std::endl;
    return false;
  }

  // PnL: SELL => +(notional - fee) = +(5.0015 - 0.0025) = 4.999
  if (!Check(Near(sell.fill_pnl, 4.999, 0.001), "fill_pnl for sell")) {
    std::cerr << "  actual fill_pnl: " << sell.fill_pnl << std::endl;
    return false;
  }

  return true;
}

bool test_batch_metrics() {
  auto batch = MakeBatch();
  auto fm = cex::backtest::app::MetricsCalculator::ComputeFillMetrics(batch);
  auto bm = cex::backtest::app::MetricsCalculator::ComputeBatchMetrics(batch, fm);

  if (!Check(bm.batch_id == "batch-m-1", "batch_id")) return false;
  if (!Check(bm.event_time_ms == 1700000000000LL, "event_time_ms")) return false;
  if (!Check(bm.num_fills == 2, "num_fills")) return false;
  if (!Check(bm.num_buy_fills == 1, "num_buy_fills")) return false;
  if (!Check(bm.num_sell_fills == 1, "num_sell_fills")) return false;

  // total_notional = 10.001 + 5.0015 = 15.0025
  if (!Check(Near(bm.total_notional, 15.0025, 0.001), "total_notional")) {
    std::cerr << "  actual: " << bm.total_notional << std::endl;
    return false;
  }

  // total_fees = 0.005 + 0.0025 = 0.0075
  if (!Check(Near(bm.total_fees, 0.0075, 0.0001), "total_fees")) {
    std::cerr << "  actual: " << bm.total_fees << std::endl;
    return false;
  }

  // net_pnl = -10.006 + 4.999 = -5.007
  if (!Check(Near(bm.net_pnl, -5.007, 0.001), "net_pnl")) {
    std::cerr << "  actual: " << bm.net_pnl << std::endl;
    return false;
  }

  // VWAP for BTC/USDT = total_notional / total_qty = 15.0025 / 0.15 = 100.016666...
  if (!Check(bm.vwap_json.find("BTC/USDT") != std::string::npos, "vwap_json has BTC/USDT")) {
    std::cerr << "  actual: " << bm.vwap_json << std::endl;
    return false;
  }

  if (!Check(Near(bm.solve_time_ms, 12.0), "solve_time_ms")) return false;
  if (!Check(Near(bm.residual_norm, 0.00042), "residual_norm")) return false;

  return true;
}

bool test_empty_batch_metrics() {
  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-empty");

  auto fm = cex::backtest::app::MetricsCalculator::ComputeFillMetrics(batch);
  auto bm = cex::backtest::app::MetricsCalculator::ComputeBatchMetrics(batch, fm);

  if (!Check(fm.empty(), "no fill metrics for empty batch")) return false;
  if (!Check(bm.num_fills == 0, "num_fills == 0")) return false;
  if (!Check(Near(bm.total_notional, 0.0), "total_notional == 0")) return false;
  if (!Check(Near(bm.net_pnl, 0.0), "net_pnl == 0")) return false;
  if (!Check(bm.vwap_json == "{}", "vwap_json is empty object")) return false;

  return true;
}

bool test_is_zero_when_clear_price_missing() {
  auto batch = MakeBatch();
  batch.mutable_clear_prices()->clear();

  auto fm = cex::backtest::app::MetricsCalculator::ComputeFillMetrics(batch);
  if (!Check(fm.size() == 2, "2 fills expected")) return false;
  if (!Check(Near(fm[0].clear_price, 0.0), "missing clear price -> 0")) return false;
  if (!Check(Near(fm[1].clear_price, 0.0), "missing clear price -> 0")) return false;
  if (!Check(Near(fm[0].is_bps, 0.0), "buy is_bps stays 0 when clear missing")) return false;
  if (!Check(Near(fm[1].is_bps, 0.0), "sell is_bps stays 0 when clear missing")) return false;
  return true;
}

bool test_vwap_zero_when_qty_is_zero() {
  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("batch-zero-qty");
  auto* f = batch.add_fills();
  f->mutable_instrument()->set_symbol("BTC/USDT");
  f->set_side(fob::common::v1::SIDE_BUY);
  f->mutable_executed_qty()->set_units(0);
  f->mutable_executed_qty()->set_scale(6);
  f->mutable_executed_notional()->set_units(1000000);
  f->mutable_executed_notional()->set_scale(6);

  auto fm = cex::backtest::app::MetricsCalculator::ComputeFillMetrics(batch);
  auto bm = cex::backtest::app::MetricsCalculator::ComputeBatchMetrics(batch, fm);
  if (!Check(bm.vwap_json.find("BTC/USDT") != std::string::npos, "symbol exists in vwap"))
    return false;
  if (!Check(bm.vwap_json.find(":0") != std::string::npos, "zero qty -> vwap 0")) return false;
  return true;
}

bool test_uc_metrics_integration() {
  // Verify that BacktestUseCases computes metrics when storage is provided.
  struct FakeStorage final : public cex::backtest::app::IBatchReplayStorage {
    bool SaveBatchResult(const fob::matching::v1::BatchResult&) override { return true; }
    bool SaveFills(const fob::matching::v1::BatchResult&) override { return true; }
  };

  struct FakeMetrics final : public cex::backtest::app::IMetricsStorage {
    int fill_calls{0};
    int batch_calls{0};
    std::string last_batch_id;
    uint32_t last_num_fills{0};

    bool SaveFillMetrics(const std::string& batch_id, int64_t,
                         const std::vector<cex::backtest::app::FillMetrics>& /*m*/) override {
      ++fill_calls;
      last_batch_id = batch_id;
      return true;
    }
    bool SaveBatchMetrics(const cex::backtest::app::BatchMetrics& bm) override {
      ++batch_calls;
      last_num_fills = bm.num_fills;
      return true;
    }
  };

  FakeStorage storage;
  FakeMetrics metrics;
  cex::backtest::app::BacktestUseCases uc(&storage, &metrics);

  auto batch = MakeBatch();
  uc.OnBatchResult(batch);

  if (!Check(metrics.fill_calls == 1, "SaveFillMetrics called once")) return false;
  if (!Check(metrics.batch_calls == 1, "SaveBatchMetrics called once")) return false;
  if (!Check(metrics.last_batch_id == "batch-m-1", "correct batch_id passed")) return false;
  if (!Check(metrics.last_num_fills == 2, "correct num_fills in batch metrics")) return false;

  auto stats = uc.GetStats();
  if (!Check(stats.metrics_computed == 1, "metrics_computed counter")) return false;

  return true;
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

  run("test_fill_metrics_count", test_fill_metrics_count);
  run("test_fill_metrics_buy", test_fill_metrics_buy);
  run("test_fill_metrics_sell", test_fill_metrics_sell);
  run("test_batch_metrics", test_batch_metrics);
  run("test_empty_batch_metrics", test_empty_batch_metrics);
  run("test_is_zero_when_clear_price_missing", test_is_zero_when_clear_price_missing);
  run("test_vwap_zero_when_qty_is_zero", test_vwap_zero_when_qty_is_zero);
  run("test_uc_metrics_integration", test_uc_metrics_integration);

  if (all_passed) {
    std::cout << "[OK] metrics_test passed (8 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
