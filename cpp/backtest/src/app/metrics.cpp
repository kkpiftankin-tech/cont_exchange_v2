#include "app/metrics.hpp"

#include <cmath>
#include <sstream>
#include <unordered_map>

#include "cex/common/decimal.hpp"

namespace cex::backtest::app {
namespace {

double DecimalToDouble(const fob::common::v1::Decimal& d) {
  return static_cast<double>(d.units()) / std::pow(10.0, d.scale());
}

int64_t TimestampToUnixMs(const google::protobuf::Timestamp& ts) {
  return static_cast<int64_t>(ts.seconds()) * 1000LL +
         static_cast<int64_t>(ts.nanos()) / 1000000LL;
}

std::string SideStr(fob::common::v1::Side side) {
  return side == fob::common::v1::SIDE_BUY ? "buy" : "sell";
}

double LookupClearPrice(
    const google::protobuf::Map<std::string, fob::common::v1::Decimal>& clear_prices,
    const std::string& symbol) {
  auto it = clear_prices.find(symbol);
  if (it == clear_prices.end()) return 0.0;
  return DecimalToDouble(it->second);
}

std::string JsonEscape(const std::string& s) {
  std::ostringstream out;
  for (char c : s) {
    if (c == '"') out << "\\\"";
    else if (c == '\\') out << "\\\\";
    else out << c;
  }
  return out.str();
}

}  // namespace

std::vector<FillMetrics> MetricsCalculator::ComputeFillMetrics(
    const fob::matching::v1::BatchResult& batch) {
  std::vector<FillMetrics> result;
  result.reserve(static_cast<size_t>(batch.fills_size()));

  for (const auto& fill : batch.fills()) {
    FillMetrics m;
    m.order_id = fill.order_id();
    m.user_id = fill.user_id();
    m.symbol = fill.instrument().symbol();
    m.side = SideStr(fill.side());

    m.executed_qty = fill.has_executed_qty() ? DecimalToDouble(fill.executed_qty()) : 0.0;
    m.price = fill.has_price() ? DecimalToDouble(fill.price()) : 0.0;
    m.executed_notional = fill.has_executed_notional()
                              ? DecimalToDouble(fill.executed_notional())
                              : 0.0;

    if (fill.has_fee() && fill.fee().has_cost()) {
      m.fee_amount = fill.fee().cost().has_amount()
                         ? DecimalToDouble(fill.fee().cost().amount())
                         : 0.0;
      m.fee_currency = fill.fee().cost().currency();
    }
    m.liquidity_source =
        fill.liquidity_source().empty()
            ? (fill.has_provenance() && !fill.provenance().liquidity_source().empty()
                   ? fill.provenance().liquidity_source()
                   : "internal")
            : fill.liquidity_source();
    if (fill.has_provenance()) {
      m.venue_id = fill.provenance().venue_id();
      m.snapshot_id = fill.provenance().snapshot_id();
      m.curve_id = fill.provenance().curve_id();
    }

    m.clear_price = LookupClearPrice(batch.clear_prices(), m.symbol);

    // IS (basis points) relative to clearing price.
    if (m.clear_price > 0.0 && m.price > 0.0) {
      if (fill.side() == fob::common::v1::SIDE_BUY) {
        // BUY: positive IS = paid more than clearing price (worse).
        m.is_bps = (m.price / m.clear_price - 1.0) * 10000.0;
      } else {
        // SELL: positive IS = received less than clearing price (worse).
        m.is_bps = (m.clear_price / m.price - 1.0) * 10000.0;
      }
    }

    // Cash-flow PnL.
    if (fill.side() == fob::common::v1::SIDE_BUY) {
      m.fill_pnl = -m.executed_notional - m.fee_amount;
    } else {
      m.fill_pnl = m.executed_notional - m.fee_amount;
    }

    result.push_back(std::move(m));
  }

  return result;
}

BatchMetrics MetricsCalculator::ComputeBatchMetrics(
    const fob::matching::v1::BatchResult& batch,
    const std::vector<FillMetrics>& fill_metrics) {
  BatchMetrics bm;
  bm.batch_id = batch.batch_id();

  if (batch.has_timestamp()) {
    bm.event_time_ms = TimestampToUnixMs(batch.timestamp());
  } else if (batch.has_meta() && batch.meta().has_ts_event()) {
    bm.event_time_ms = TimestampToUnixMs(batch.meta().ts_event());
  }

  bm.num_fills = static_cast<uint32_t>(fill_metrics.size());

  if (batch.has_diagnostics()) {
    bm.solve_time_ms = batch.diagnostics().solve_time_ms();
    bm.residual_norm = batch.diagnostics().residual_norm();
  }

  // VWAP accumulator: symbol -> (sum_notional, sum_qty).
  struct VwapAcc {
    double sum_notional{0};
    double sum_qty{0};
  };
  std::unordered_map<std::string, VwapAcc> vwap_acc;

  for (const auto& fm : fill_metrics) {
    bm.total_notional += fm.executed_notional;
    bm.total_fees += fm.fee_amount;
    bm.net_pnl += fm.fill_pnl;

    if (fm.side == "buy") {
      ++bm.num_buy_fills;
      bm.buy_notional += fm.executed_notional;
    } else {
      ++bm.num_sell_fills;
      bm.sell_notional += fm.executed_notional;
    }

    auto& acc = vwap_acc[fm.symbol];
    acc.sum_notional += fm.executed_notional;
    acc.sum_qty += fm.executed_qty;
  }

  // Build VWAP JSON.
  std::ostringstream vwap;
  vwap << "{";
  bool first = true;
  for (const auto& [symbol, acc] : vwap_acc) {
    if (!first) vwap << ",";
    first = false;
    const double vwap_price = acc.sum_qty > 0.0 ? acc.sum_notional / acc.sum_qty : 0.0;
    vwap << "\"" << JsonEscape(symbol) << "\":" << vwap_price;
  }
  vwap << "}";
  bm.vwap_json = vwap.str();

  return bm;
}

}  // namespace cex::backtest::app
