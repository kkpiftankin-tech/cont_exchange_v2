#include "app/market_data_uc.hpp"

#include "cex/common/log.hpp"
#include "infra/clickhouse/clickhouse_liquidity_curve_storage.hpp"

namespace cex::market_data::app {

MarketDataUseCases::MarketDataUseCases(
    IBatchOutputsStorage* batch_storage, IExecutionVenueStorage* execution_storage,
    domain::IOrderBookStorage* ob_storage,
    domain::IOrderBookPublisher* publisher, domain::ILiquidityCurveStorage* memory_curve_storage,
    infra::clickhouse::ClickHouseLiquidityCurveStorage* ch_curve_storage)
    : batch_storage_(batch_storage),
      execution_storage_(execution_storage),
      memory_curve_storage_(memory_curve_storage),
      ch_curve_storage_(ch_curve_storage),
      update_uc_(ob_storage, publisher) {}

std::string MarketDataUseCases::key(const std::string& venue, const std::string& symbol) {
  return venue + "|" + symbol;
}

std::string MarketDataUseCases::curve_key(const std::string& venue, const std::string& symbol,
                                          fob::venue::v1::ExecutionSide side) {
  std::string side_str = (side == fob::venue::v1::EXECUTION_SIDE_BUY) ? "BUY" : "SELL";
  return venue + "|" + symbol + "|" + side_str;
}

void MarketDataUseCases::OnMarketDataRaw(const fob::marketdata::v1::MarketDataRaw& evt) {
  if (!evt.has_ticker()) return;
  std::lock_guard<std::mutex> lg(mu_);
  const auto& t = evt.ticker();
  last_ticker_[key(t.venue(), t.instrument().symbol())] = t;
  update_uc_.OnMarketDataRaw(evt);
}

void MarketDataUseCases::OnBatchResult(const fob::matching::v1::BatchResult& batch) {
  {
    std::lock_guard<std::mutex> lg(mu_);
    ++batches_processed_;
    fills_processed_ += static_cast<uint64_t>(batch.fills_size());
    last_batch_id_ = batch.batch_id();
  }

  if (batch_storage_ == nullptr) {
    cex::common::log_json("WARN", "Batch output storage is not configured",
                          {{"batch_id", batch.batch_id()}});
    return;
  }

  const bool batch_ok = batch_storage_->SaveBatchResult(batch);
  const bool fills_ok = batch_storage_->SaveFills(batch);
  cex::common::log_json("INFO", "MarketData processed batch.outputs",
                        {{"batch_id", batch.batch_id()},
                         {"clear_prices", std::to_string(batch.clear_prices_size())},
                         {"executed_rates", std::to_string(batch.executed_rates_size())},
                         {"fills", std::to_string(batch.fills_size())},
                         {"batch_saved", batch_ok ? "true" : "false"},
                         {"fills_saved", fills_ok ? "true" : "false"}});
  if (!batch_ok || !fills_ok) {
    cex::common::log_json("ERROR", "Failed to persist batch output",
                          {{"batch_id", batch.batch_id()},
                           {"batch_saved", batch_ok ? "true" : "false"},
                           {"fills_saved", fills_ok ? "true" : "false"}});
  }
  update_uc_.OnBatchResult(batch);
}

void MarketDataUseCases::OnExecutionReport(
    const fob::execution::v1::ExecutionReport& report) {
  if (execution_storage_ == nullptr) {
    cex::common::log_json("WARN", "Execution report storage is not configured",
                          {{"intent_id", report.intent_id()},
                           {"report_id", report.report_id()}});
    return;
  }
  const bool ok = execution_storage_->SaveExecutionReport(report);
  cex::common::log_json(ok ? "INFO" : "ERROR", "MarketData processed execution.venue",
                        {{"intent_id", report.intent_id()},
                         {"report_id", report.report_id()},
                         {"venue", report.venue()},
                         {"symbol", report.instrument().symbol()},
                         {"stored", ok ? "true" : "false"}});
}

void MarketDataUseCases::OnLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve) {
  // Save to in-memory storage for fast access
  if (memory_curve_storage_ != nullptr) {
    memory_curve_storage_->Store(curve);
  } else {
    cex::common::log_json("WARN", "Memory curve storage is not configured",
                          {{"venue", curve.venue_id()}, {"symbol", curve.instrument().symbol()}});
  }

  // Save to ClickHouse for historical analytics
  if (ch_curve_storage_ != nullptr) {
    ch_curve_storage_->Save(curve);
  } else {
    cex::common::log_json("WARN", "ClickHouse curve storage is not configured",
                          {{"venue", curve.venue_id()}, {"symbol", curve.instrument().symbol()}});
  }

  cex::common::log_json("INFO", "Stored liquidity curve",
                        {{"service", "market_data"},
                         {"component", "market_data_service"},
                         {"participant", "Market Data Service"},
                         {"stage", "store_liquidity_curve"},
                         {"topic", "venue.liquidity.fob"},
                         {"venue", curve.venue_id()},
                         {"symbol", curve.instrument().symbol()},
                         {"curve_id", curve.curve_id()},
                         {"snapshot_id", curve.snapshot_id()},
                         {"confidence", std::to_string(curve.confidence())},
                         {"level", curve.level()},
                         {"has_bid", curve.has_bid_curve() ? "true" : "false"},
                         {"has_ask", curve.has_ask_curve() ? "true" : "false"},
                         {"memory_store_enabled",
                          memory_curve_storage_ != nullptr ? "true" : "false"},
                         {"clickhouse_store_enabled",
                          ch_curve_storage_ != nullptr ? "true" : "false"},
                         {"source_file", "cpp/market_data/src/app/market_data_uc.cpp"}});
}

void MarketDataUseCases::OnVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) {
  update_uc_.OnVenueSnapshot(snapshot);
}

std::optional<fob::marketdata::v1::Ticker> MarketDataUseCases::GetLastTicker(
    const std::string& venue, const std::string& symbol) const {
  std::lock_guard<std::mutex> lg(mu_);
  auto it = last_ticker_.find(key(venue, symbol));
  if (it == last_ticker_.end()) return std::nullopt;
  return it->second;
}

std::optional<fob::venue::v1::SideLiquidityCurve> MarketDataUseCases::GetLiquidityCurve(
    const std::string& venue, const std::string& symbol, fob::venue::v1::ExecutionSide side) const {
  if (memory_curve_storage_ == nullptr) {
    return std::nullopt;
  }
  return memory_curve_storage_->GetCurve(venue, symbol, side);
}

MarketDataUseCases::BatchOutputsStats MarketDataUseCases::GetBatchOutputsStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return BatchOutputsStats{
      .batches_processed = batches_processed_,
      .fills_processed = fills_processed_,
      .last_batch_id = last_batch_id_,
  };
}

}  // namespace cex::market_data::app
