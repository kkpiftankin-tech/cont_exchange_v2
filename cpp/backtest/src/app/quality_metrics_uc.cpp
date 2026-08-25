#include "app/quality_metrics_uc.hpp"

#include "cex/common/log.hpp"

namespace cex::backtest::app {

QualityMetricsUseCases::QualityMetricsUseCases(IReplayReader* reader,
                                               IQualityMetricsStorage* storage)
    : reader_(reader), storage_(storage) {}

VenueQualityReport QualityMetricsUseCases::ComputeReport(
    const QualityConfig& config) {
  VenueQualityReport empty;
  empty.venue_id = config.venue_id;
  empty.symbol = config.symbol;
  empty.from_ms = config.from_ms;
  empty.to_ms = config.to_ms;

  if (reader_ == nullptr) {
    cex::common::log_json("WARN", "Quality metrics reader is not configured");
    return empty;
  }

  auto curves = reader_->LoadCurves(
      config.venue_id, config.symbol, config.from_ms, config.to_ms);
  auto snapshots = reader_->LoadSnapshots(
      config.venue_id, config.symbol, config.from_ms, config.to_ms);

  auto report = QualityMetricsCalculator::BuildReport(
      config.venue_id, config.symbol, config.from_ms, config.to_ms,
      curves, snapshots, config.expected_interval_ms);

  {
    std::lock_guard<std::mutex> lg(mu_);
    ++reports_computed_;
    last_venue_id_ = config.venue_id;
    last_symbol_ = config.symbol;
  }

  cex::common::log_json("INFO", "Quality report computed",
                        {{"venue_id", config.venue_id},
                         {"symbol", config.symbol},
                         {"total_curves", std::to_string(report.total_curves)},
                         {"total_snapshots", std::to_string(report.total_snapshots)},
                         {"stale_rate", std::to_string(report.stale_rate)},
                         {"uptime", std::to_string(report.uptime)}});

  return report;
}

VenueQualityReport QualityMetricsUseCases::ComputeAndSaveReport(
    const QualityConfig& config) {
  auto report = ComputeReport(config);

  if (storage_ != nullptr) {
    const bool ok = storage_->SaveQualityReport(report);
    if (ok) {
      std::lock_guard<std::mutex> lg(mu_);
      ++reports_saved_;
    } else {
      cex::common::log_json("ERROR", "Failed to persist quality report",
                            {{"venue_id", config.venue_id},
                             {"symbol", config.symbol}});
    }
  }

  return report;
}

QualityMetricsUseCases::Stats QualityMetricsUseCases::GetStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return Stats{
      .reports_computed = reports_computed_,
      .reports_saved = reports_saved_,
      .last_venue_id = last_venue_id_,
      .last_symbol = last_symbol_,
  };
}

}  // namespace cex::backtest::app
