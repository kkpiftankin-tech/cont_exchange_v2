// ============================================================================
// vectorize.cpp — F-05A (T-F05A-202). См. заголовок в .hpp.
// ============================================================================

#include "domain/vectorize.hpp"

#include <cmath>   // std::isfinite, std::llround, std::pow
#include <set>

#include "domain/effective_price.hpp"

namespace cex::market_data::domain {

namespace {

/// double → Decimal{units, scale} (детерминировано, llround). §9-граница: solver
/// input (P_eff, d_hl) квантуется для хранения; ledger-суммы здесь не считаются.
cex::common::Decimal Quantize(double value, std::int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};
  const double factor = std::pow(10.0, static_cast<double>(scale));
  return cex::common::Decimal{static_cast<std::int64_t>(std::llround(value * factor)),
                              scale};
}

bool Positive(const cex::common::Decimal& d) {
  return cex::common::Decimal::cmp(d, cex::common::Decimal{0, d.scale}) > 0;
}

}  // namespace

AssetBasis BuildAssetBasis(const std::vector<ExternalOrderLevel>& levels) {
  std::set<std::string> assets;  // отсортированный union → детерминизм
  for (const auto& lvl : levels) {
    if (!lvl.base_asset.empty()) assets.insert(lvl.base_asset);
    if (!lvl.quote_asset.empty()) assets.insert(lvl.quote_asset);
  }
  AssetBasis basis;
  basis.assets.reserve(assets.size());
  int idx = 0;
  for (const auto& a : assets) {
    basis.assets.push_back(a);
    basis.index_of.emplace(a, idx);
    ++idx;
  }
  basis.num_assets = idx;
  return basis;
}

VectorizeResult Vectorize(const std::vector<ExternalOrderLevel>& levels,
                          const VectorizeConfig& cfg) {
  VectorizeResult out;
  out.basis = BuildAssetBasis(levels);
  const int N = out.basis.num_assets;

  for (const auto& lvl : levels) {
    // q_max = remaining, иначе quantity. Пропуск при q_max <= 0.
    const cex::common::Decimal q_max =
        Positive(lvl.remaining_quantity) ? lvl.remaining_quantity : lvl.quantity;
    if (!Positive(q_max)) {
      out.skipped.push_back({lvl.source_order_id, "non_positive_quantity"});
      continue;
    }
    if (lvl.base_asset == lvl.quote_asset || lvl.base_asset.empty() ||
        lvl.quote_asset.empty()) {
      out.skipped.push_back({lvl.source_order_id, "invalid_pair"});
      continue;
    }
    const int ix = out.basis.IndexOf(lvl.base_asset);
    const int iy = out.basis.IndexOf(lvl.quote_asset);
    if (ix < 0 || iy < 0) {
      out.skipped.push_back({lvl.source_order_id, "asset_not_in_basis"});
      continue;
    }

    const double p_eff = EffectivePriceDouble(lvl);
    if (!std::isfinite(p_eff) || p_eff <= 0.0) {
      out.skipped.push_back({lvl.source_order_id, "invalid_effective_price"});
      continue;
    }

    VectorFlowSegment seg;
    seg.source_order_id = lvl.source_order_id;
    seg.venue_id = lvl.venue_id;
    seg.pair = lvl.pair;
    seg.side = lvl.side;
    // Детерминированный id (без random/таймера): venue|source|side.
    seg.segment_id = lvl.venue_id + "|" + lvl.source_order_id + "|" +
                     ToString(lvl.side);

    // w_i (R-F05A-001, инвариант знака bid/ask).
    seg.w.assign(static_cast<std::size_t>(N), 0.0);
    if (lvl.side == LevelSide::kBid) {
      seg.w[static_cast<std::size_t>(ix)] = 1.0;
      seg.w[static_cast<std::size_t>(iy)] = -p_eff;
    } else {  // kAsk
      seg.w[static_cast<std::size_t>(ix)] = -1.0;
      seg.w[static_cast<std::size_t>(iy)] = p_eff;
    }

    // d_hl = dHL-policy(P_eff); p_low=0; p_high=d_hl.
    const double d_hl_d = cfg.dhl_fraction * p_eff;
    seg.d_hl = Quantize(d_hl_d, cfg.decimal_scale);
    seg.p_low = cex::common::Decimal{0, cfg.decimal_scale};
    seg.p_high = seg.d_hl;
    seg.effective_price = Quantize(p_eff, cfg.decimal_scale);

    // q_max, q_rate (capped).
    seg.q_max = q_max;
    if (cfg.rate_cap.units != 0 &&
        cex::common::Decimal::cmp(cfg.rate_cap, q_max) < 0) {
      seg.q_rate = cfg.rate_cap;
    } else {
      seg.q_rate = q_max;
    }

    seg.source_timestamp_ms = lvl.ts_event_ms;
    out.segments.push_back(std::move(seg));
  }

  return out;
}

}  // namespace cex::market_data::domain
