#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cex::backtest::app {

// Row returned from venue_snapshots table for replay.
struct SnapshotRow {
  std::string venue_id;
  std::string symbol;
  int64_t event_time_ms{0};
  double best_bid{0};
  double best_ask{0};
  double mid_price{0};
  double spread{0};
  // bid_depth_json / ask_depth_json: [[price, qty], ...]
  std::string bid_depth_json;
  std::string ask_depth_json;
  double tick_size{0};
  double lot_size{0};
  std::string status;
};

// Row returned from venue_liquidity_curves table for replay.
struct CurveRow {
  std::string venue_id;
  std::string symbol;
  int64_t event_time_ms{0};
  std::string snapshot_id;
  std::string curve_id;
  std::string level;
  // JSON arrays: "[1.0, 2.0, ...]"
  std::string bid_q_grid;
  std::string bid_p_of_q;
  std::string bid_s_of_q;
  std::string ask_q_grid;
  std::string ask_p_of_q;
  std::string ask_s_of_q;
  double epsilon1{0};
  double epsilon2{0};
  double epsilon3{0};
  double confidence{0};
  double mid_price{0};
  double tau_ms{0};
};

// Port for reading historical venue data from ClickHouse for replay.
class IReplayReader {
 public:
  virtual ~IReplayReader() = default;

  // Load snapshots for a given venue/symbol in a time range, ordered by time.
  virtual std::vector<SnapshotRow> LoadSnapshots(
      const std::string& venue_id,
      const std::string& symbol,
      int64_t from_ms,
      int64_t to_ms) = 0;

  // Load stored curves for a given venue/symbol in a time range, ordered by time.
  virtual std::vector<CurveRow> LoadCurves(
      const std::string& venue_id,
      const std::string& symbol,
      int64_t from_ms,
      int64_t to_ms) = 0;
};

}  // namespace cex::backtest::app
