#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fob/matching/v1/batch.pb.h"

namespace cex::backtest::app {

// Per-fill execution quality metrics.
struct FillMetrics {
  std::string order_id;
  std::string user_id;
  std::string symbol;
  std::string side;       // "buy" / "sell"

  double executed_qty{0};
  double price{0};        // actual execution price
  double executed_notional{0};
  double fee_amount{0};
  std::string fee_currency;
  std::string liquidity_source;
  std::string venue_id;
  std::string snapshot_id;
  std::string curve_id;

  double clear_price{0};  // batch clearing price for this instrument

  // Implementation Shortfall vs clearing price (basis points).
  // Positive = execution was worse than clearing price.
  // BUY: (exec_price / clear_price - 1) * 10000
  // SELL: (clear_price / exec_price - 1) * 10000
  double is_bps{0};

  // Realized cash-flow PnL for this fill (in quote currency).
  // BUY:  -executed_notional - fee
  // SELL: +executed_notional - fee
  double fill_pnl{0};
};

// Per-batch aggregate metrics.
struct BatchMetrics {
  std::string batch_id;
  int64_t event_time_ms{0};

  uint32_t num_fills{0};
  uint32_t num_buy_fills{0};
  uint32_t num_sell_fills{0};

  double total_notional{0};
  double buy_notional{0};
  double sell_notional{0};
  double total_fees{0};
  double net_pnl{0};

  // Volume-weighted average price per instrument (JSON: {"BTC/USDT":"100.01"}).
  std::string vwap_json;

  double solve_time_ms{0};
  double residual_norm{0};
};

// Pure domain calculator — no I/O, fully testable.
struct MetricsCalculator {
  static std::vector<FillMetrics> ComputeFillMetrics(
      const fob::matching::v1::BatchResult& batch);

  static BatchMetrics ComputeBatchMetrics(
      const fob::matching::v1::BatchResult& batch,
      const std::vector<FillMetrics>& fill_metrics);
};

}  // namespace cex::backtest::app
