#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "app/replay_metrics.hpp"
#include "app/replay_reader_port.hpp"

namespace cex::backtest::app {

// Configuration for a single replay run.
struct ReplayConfig {
  std::string venue_id;
  std::string symbol;
  int64_t from_ms{0};
  int64_t to_ms{0};
  double tau_ms{100.0};
};

// Replays historical LOB→FOB pipeline on stored VenueSnapshots.
// Rebuilds curves at L1/L2/L3 from raw depth data, then compares
// against originally stored VenueLiquidityCurves.
class LobFobReplayUseCases {
 public:
  struct Stats {
    uint64_t replays_run{0};
    uint64_t snapshots_processed{0};
    uint64_t comparisons_produced{0};
    std::string last_venue_id;
    std::string last_symbol;
  };

  explicit LobFobReplayUseCases(IReplayReader* reader = nullptr);

  // Run a replay: load snapshots, rebuild curves, compare with stored originals.
  // Returns per-snapshot comparisons at L1/L2/L3.
  std::vector<ReplayCurveComparison> RunReplay(const ReplayConfig& config);

  // Run a replay and aggregate into summary metrics.
  ReplayRunMetrics RunReplayWithMetrics(const ReplayConfig& config);

  Stats GetStats() const;

 private:
  // Parse "[[price,qty],...]" JSON into vectors of doubles.
  static std::vector<std::pair<double, double>> ParseDepthJson(
      const std::string& json);

  // Parse "[1.0, 2.0, ...]" JSON into a vector of doubles.
  static std::vector<double> ParseDoubleArray(const std::string& json);

  IReplayReader* reader_{nullptr};
  mutable std::mutex mu_;
  uint64_t replays_run_{0};
  uint64_t snapshots_processed_{0};
  uint64_t comparisons_produced_{0};
  std::string last_venue_id_;
  std::string last_symbol_;
};

}  // namespace cex::backtest::app
