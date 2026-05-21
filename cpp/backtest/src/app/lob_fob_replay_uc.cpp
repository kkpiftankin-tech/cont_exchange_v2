#include "app/lob_fob_replay_uc.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

// Venues domain: canonicalization and curve building.
#include "domain/depth_canonicalizer.hpp"
#include "domain/depth_curve_builder.hpp"

namespace cex::backtest::app {
namespace {

using cex::common::Decimal;
namespace vdomain = cex::venues::domain;

// Convert internal DepthSideCurves p_of_q to a double vector for MAE comparison.
std::vector<double> ExtractPOfQ(const vdomain::DepthSideCurves& curves) {
  std::vector<double> result;
  result.reserve(curves.p_of_q.size());
  for (const auto& pt : curves.p_of_q) {
    result.push_back(static_cast<double>(pt.price));
  }
  return result;
}

// Convert internal DepthSideCurves s_of_q to a double vector for MAE comparison.
std::vector<double> ExtractSOfQ(const vdomain::DepthSideCurves& curves) {
  std::vector<double> result;
  result.reserve(curves.s_of_q.size());
  for (const auto& pt : curves.s_of_q) {
    result.push_back(static_cast<double>(pt.cumulative_cost));
  }
  return result;
}

// Build BookLevels from parsed depth pairs.
std::vector<vdomain::BookLevel> MakeBookLevels(
    const std::vector<std::pair<double, double>>& depth) {
  std::vector<vdomain::BookLevel> levels;
  levels.reserve(depth.size());
  for (const auto& [price, qty] : depth) {
    // Convert doubles to Decimal with scale=8 for reasonable precision.
    const int32_t scale = 8;
    levels.push_back(vdomain::BookLevel{
        .price = Decimal{static_cast<int64_t>(std::round(price * 1e8)), scale},
        .qty = Decimal{static_cast<int64_t>(std::round(qty * 1e8)), scale},
    });
  }
  return levels;
}

// Build L1 curves from canonical book for a given side.
vdomain::DepthSideCurves BuildL1(const vdomain::CanonicalOrderBook& book,
                                 vdomain::ExecutionSide side,
                                 const Decimal& tau_sec) {
  return vdomain::BuildDepthSideCurves(book, side, tau_sec);
}

// Build L2 curves: L1 + convexification + Moreau regularization.
vdomain::DepthSideCurves BuildL2(const vdomain::CanonicalOrderBook& book,
                                 vdomain::ExecutionSide side,
                                 const Decimal& tau_sec) {
  auto curves = vdomain::BuildDepthSideCurves(book, side, tau_sec);
  if (curves.empty()) return curves;
  curves = vdomain::ApplyConvexifiedCostLayer(curves);
  curves = vdomain::ApplyMoreauRegularizationL2(curves);
  return curves;
}

// Build L3 curves: L2 + Fenchel-Legendre dual layer.
vdomain::DepthSideCurves BuildL3(const vdomain::CanonicalOrderBook& book,
                                 vdomain::ExecutionSide side,
                                 const Decimal& tau_sec) {
  auto curves = BuildL2(book, side, tau_sec);
  if (curves.empty()) return curves;
  curves = vdomain::ApplyFenchelLegendreLayer(curves);
  return curves;
}

// Compute epsilon1: relative MAE of replayed S(q) vs original S(q).
double ComputeEpsilon1(const std::vector<double>& replayed_s,
                       const std::vector<double>& original_s) {
  const double mae = ReplayMetricsCalculator::ComputeMAE(replayed_s, original_s);
  // Normalize by mean of original if non-zero.
  double original_mean = 0;
  for (double v : original_s) original_mean += std::fabs(v);
  if (!original_s.empty()) original_mean /= static_cast<double>(original_s.size());
  return (original_mean > 1e-12) ? mae / original_mean : mae;
}

// Compute epsilon2: relative MAE of replayed p(q) vs original p(q).
double ComputeEpsilon2(const std::vector<double>& replayed_p,
                       const std::vector<double>& original_p) {
  const double mae = ReplayMetricsCalculator::ComputeMAE(replayed_p, original_p);
  double original_mean = 0;
  for (double v : original_p) original_mean += std::fabs(v);
  if (!original_p.empty()) original_mean /= static_cast<double>(original_p.size());
  return (original_mean > 1e-12) ? mae / original_mean : mae;
}

}  // namespace

LobFobReplayUseCases::LobFobReplayUseCases(IReplayReader* reader)
    : reader_(reader) {}

std::vector<std::pair<double, double>> LobFobReplayUseCases::ParseDepthJson(
    const std::string& json) {
  std::vector<std::pair<double, double>> result;
  // Parse "[[price,qty],[price,qty],...]"
  std::size_t pos = 0;
  while (pos < json.size()) {
    // Find next inner '['.
    auto open = json.find('[', pos);
    if (open == std::string::npos) break;
    // Skip if it's the outer '['.
    if (pos == 0 && open == 0) { pos = 1; continue; }
    auto comma = json.find(',', open + 1);
    auto close = json.find(']', open + 1);
    if (comma == std::string::npos || close == std::string::npos) break;
    double price = std::stod(json.substr(open + 1, comma - open - 1));
    double qty = std::stod(json.substr(comma + 1, close - comma - 1));
    result.push_back({price, qty});
    pos = close + 1;
  }
  return result;
}

std::vector<double> LobFobReplayUseCases::ParseDoubleArray(
    const std::string& json) {
  std::vector<double> result;
  // Parse "[1.0, 2.0, ...]"
  std::size_t start = json.find('[');
  std::size_t end = json.rfind(']');
  if (start == std::string::npos || end == std::string::npos || end <= start + 1) {
    return result;
  }
  std::string inner = json.substr(start + 1, end - start - 1);
  std::istringstream ss(inner);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) continue;
    result.push_back(std::stod(token));
  }
  return result;
}

std::vector<ReplayCurveComparison> LobFobReplayUseCases::RunReplay(
    const ReplayConfig& config) {
  std::vector<ReplayCurveComparison> comparisons;

  if (reader_ == nullptr) {
    cex::common::log_json("WARN", "Replay reader is not configured");
    return comparisons;
  }

  // Load historical data.
  auto snapshots = reader_->LoadSnapshots(
      config.venue_id, config.symbol, config.from_ms, config.to_ms);
  auto stored_curves = reader_->LoadCurves(
      config.venue_id, config.symbol, config.from_ms, config.to_ms);

  // Index stored curves by (event_time_ms, level) for fast lookup.
  std::unordered_map<std::string, const CurveRow*> curve_index;
  for (const auto& c : stored_curves) {
    curve_index[std::to_string(c.event_time_ms) + ":" + c.level] = &c;
  }

  const Decimal tau_sec{
      static_cast<int64_t>(std::round(config.tau_ms * 1e3)), 6};

  vdomain::DepthCanonicalizationConfig canon_cfg;

  for (const auto& snap : snapshots) {
    // Parse depth from stored JSON.
    auto bid_depth = ParseDepthJson(snap.bid_depth_json);
    auto ask_depth = ParseDepthJson(snap.ask_depth_json);

    if (bid_depth.empty() && ask_depth.empty()) continue;

    // Set tick_size/lot_size from snapshot.
    if (snap.tick_size > 0) {
      canon_cfg.tick_size = Decimal{
          static_cast<int64_t>(std::round(snap.tick_size * 1e8)), 8};
    }
    if (snap.lot_size > 0) {
      canon_cfg.lot_size = Decimal{
          static_cast<int64_t>(std::round(snap.lot_size * 1e8)), 8};
    }

    // Build canonical order book.
    auto bids = MakeBookLevels(bid_depth);
    auto asks = MakeBookLevels(ask_depth);
    auto book = vdomain::CanonicalizeOrderBook(bids, asks, canon_cfg);

    if (book.is_empty()) continue;

    // Build replayed curves at L1/L2/L3.
    struct LevelResult {
      std::string name;
      vdomain::DepthSideCurves bid_curves;
      vdomain::DepthSideCurves ask_curves;
    };

    std::vector<LevelResult> levels = {
        {"L1",
         BuildL1(book, vdomain::ExecutionSide::kSell, tau_sec),
         BuildL1(book, vdomain::ExecutionSide::kBuy, tau_sec)},
        {"L2",
         BuildL2(book, vdomain::ExecutionSide::kSell, tau_sec),
         BuildL2(book, vdomain::ExecutionSide::kBuy, tau_sec)},
        {"L3",
         BuildL3(book, vdomain::ExecutionSide::kSell, tau_sec),
         BuildL3(book, vdomain::ExecutionSide::kBuy, tau_sec)},
    };

    for (auto& [level_name, bid_curves, ask_curves] : levels) {
      ReplayCurveComparison cmp;
      cmp.venue_id = snap.venue_id;
      cmp.symbol = snap.symbol;
      cmp.event_time_ms = snap.event_time_ms;
      cmp.level = level_name;

      auto replayed_bid_p = ExtractPOfQ(bid_curves);
      auto replayed_ask_p = ExtractPOfQ(ask_curves);
      auto replayed_bid_s = ExtractSOfQ(bid_curves);
      auto replayed_ask_s = ExtractSOfQ(ask_curves);

      // Look up original stored curve for this timestamp+level.
      auto key = std::to_string(snap.event_time_ms) + ":" + level_name;
      auto it = curve_index.find(key);
      if (it != curve_index.end()) {
        const auto& orig = *it->second;
        cmp.matched = true;
        cmp.original_epsilon1 = orig.epsilon1;
        cmp.original_epsilon2 = orig.epsilon2;
        cmp.original_epsilon3 = orig.epsilon3;
        cmp.original_confidence = orig.confidence;

        auto orig_bid_p = ParseDoubleArray(orig.bid_p_of_q);
        auto orig_ask_p = ParseDoubleArray(orig.ask_p_of_q);
        auto orig_bid_s = ParseDoubleArray(orig.bid_s_of_q);
        auto orig_ask_s = ParseDoubleArray(orig.ask_s_of_q);

        cmp.bid_p_of_q_mae = ReplayMetricsCalculator::ComputeMAE(
            replayed_bid_p, orig_bid_p);
        cmp.ask_p_of_q_mae = ReplayMetricsCalculator::ComputeMAE(
            replayed_ask_p, orig_ask_p);
        cmp.bid_s_of_q_mae = ReplayMetricsCalculator::ComputeMAE(
            replayed_bid_s, orig_bid_s);
        cmp.ask_s_of_q_mae = ReplayMetricsCalculator::ComputeMAE(
            replayed_ask_s, orig_ask_s);

        cmp.replayed_epsilon1 = ComputeEpsilon1(replayed_bid_s, orig_bid_s);
        cmp.replayed_epsilon2 = ComputeEpsilon2(replayed_bid_p, orig_bid_p);
      } else {
        // No matching stored curve — still produce comparison with zeros.
        cmp.matched = false;
      }

      comparisons.push_back(std::move(cmp));
    }
  }

  {
    std::lock_guard<std::mutex> lg(mu_);
    ++replays_run_;
    snapshots_processed_ += snapshots.size();
    comparisons_produced_ += comparisons.size();
    last_venue_id_ = config.venue_id;
    last_symbol_ = config.symbol;
  }

  cex::common::log_json("INFO", "LOB->FOB replay completed",
                        {{"venue_id", config.venue_id},
                         {"symbol", config.symbol},
                         {"snapshots", std::to_string(snapshots.size())},
                         {"comparisons", std::to_string(comparisons.size())}});

  return comparisons;
}

ReplayRunMetrics LobFobReplayUseCases::RunReplayWithMetrics(
    const ReplayConfig& config) {
  auto comparisons = RunReplay(config);
  auto metrics = ReplayMetricsCalculator::Aggregate(
      config.venue_id, config.symbol, config.from_ms, config.to_ms,
      comparisons);
  metrics.snapshots_processed = static_cast<uint32_t>(
      comparisons.size() / 3);  // 3 levels per snapshot.
  return metrics;
}

LobFobReplayUseCases::Stats LobFobReplayUseCases::GetStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return Stats{
      .replays_run = replays_run_,
      .snapshots_processed = snapshots_processed_,
      .comparisons_produced = comparisons_produced_,
      .last_venue_id = last_venue_id_,
      .last_symbol = last_symbol_,
  };
}

}  // namespace cex::backtest::app
