// ============================================================================
// curve_to_levels.cpp — F-05A (T-F05A-205). См. заголовок в .hpp.
// ============================================================================

#include "app/curve_to_levels.hpp"

#include <algorithm>  // std::min
#include <cmath>      // std::isfinite, std::llround, std::pow, std::fabs
#include <limits>     // ADR-053: std::numeric_limits для α = min D/|δ|
#include <set>        // ADR-052: общий basis (отсортированное объединение)
#include <string>
#include <utility>    // ADR-053: std::pair для сырых уровней
#include <vector>

#include "cex/common/env.hpp"  // ADR-053: F05A_TRANSLATOR_MODEL / F05A_SAFE_SHARE

namespace cex::market_data::app {

namespace {

cex::common::Decimal Quantize(double value, std::int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};
  const double factor = std::pow(10.0, static_cast<double>(scale));
  return cex::common::Decimal{static_cast<std::int64_t>(std::llround(value * factor)),
                              scale};
}

// ADR-053 safe-translator: α = min_k D_k/|δ_k| для одной стороны FOB-кривой.
// D_k — накопл. quote-notional по VWAP-глубине; δ_k = 1e4·|ln(vwap_k/mid)| (bps).
// Возвращает +inf, если валидных уровней нет.
double SideAlphaExt(const fob::venue::v1::SideLiquidityCurve& side, double mid) {
  const int n = std::min(side.q_grid_size(), side.p_of_q_size());
  double prev_q = 0.0, cum_notional = 0.0;
  double alpha = std::numeric_limits<double>::infinity();
  for (int k = 0; k < n; ++k) {
    const double q_cum = side.q_grid(k);
    const double price = side.p_of_q(k);
    const double step = q_cum - prev_q;
    prev_q = q_cum;
    if (!std::isfinite(step) || step <= 0.0) continue;
    if (!std::isfinite(price) || price <= 0.0) continue;
    if (!(q_cum > 0.0)) continue;
    cum_notional += price * step;
    const double vwap = cum_notional / q_cum;
    if (!(vwap > 0.0) || !(mid > 0.0)) continue;
    const double delta_bps = 10000.0 * std::fabs(std::log(vwap / mid));
    if (delta_bps <= 1e-9) continue;                 // на топе δ→0 — пропускаем
    const double a = cum_notional / delta_bps;       // D_k/|δ_k|
    if (std::isfinite(a) && a > 0.0 && a < alpha) alpha = a;
  }
  return alpha;
}

std::int64_t TsToMs(const google::protobuf::Timestamp& ts) {
  return ts.seconds() * 1000 + ts.nanos() / 1000000;
}

/// Разложить одну сторону кривой в уровни. side задаёт знак/семантику.
void AppendSide(const fob::venue::v1::SideLiquidityCurve& side_curve,
                domain::LevelSide side, const fob::venue::v1::VenueLiquidityCurve& curve,
                std::int32_t scale, std::vector<domain::ExternalOrderLevel>& out) {
  const int n =
      std::min(side_curve.q_grid_size(), side_curve.p_of_q_size());
  double prev_q = 0.0;
  for (int k = 0; k < n; ++k) {
    const double q_cum = side_curve.q_grid(k);
    const double price = side_curve.p_of_q(k);
    const double step_qty = q_cum - prev_q;
    prev_q = q_cum;
    if (!std::isfinite(step_qty) || step_qty <= 0.0) continue;
    if (!std::isfinite(price) || price <= 0.0) continue;

    domain::ExternalOrderLevel lvl;
    lvl.venue_id = curve.venue_id();
    lvl.pair = curve.instrument().symbol();
    lvl.base_asset = curve.instrument().base();
    lvl.quote_asset = curve.instrument().quote();
    lvl.side = side;
    lvl.source_order_id = curve.venue_id() + "|" + lvl.pair + "|" +
                          domain::ToString(side) + "|" + std::to_string(k);
    lvl.price = Quantize(price, scale);
    lvl.quantity = Quantize(step_qty, scale);
    lvl.remaining_quantity = lvl.quantity;
    lvl.ts_event_ms = TsToMs(curve.timestamp());
    out.push_back(std::move(lvl));
  }
}

/// ADR-051: свернуть одну сторону кривой в ОДИН линейный сегмент.
void AppendLinearSide(const fob::venue::v1::SideLiquidityCurve& side_curve,
                      domain::LevelSide side,
                      const fob::venue::v1::VenueLiquidityCurve& curve,
                      std::int32_t scale, std::vector<domain::ExternalOrderLevel>& out) {
  const int n = std::min(side_curve.q_grid_size(), side_curve.p_of_q_size());
  if (n < 1) return;
  const double best_price = side_curve.p_of_q(0);      // a = top-of-book
  const double worst_price = side_curve.p_of_q(n - 1);
  const double q_max = side_curve.q_grid(n - 1);       // полная глубина стороны
  if (!std::isfinite(best_price) || best_price <= 0.0) return;
  if (!std::isfinite(q_max) || q_max <= 0.0) return;

  domain::ExternalOrderLevel lvl;
  lvl.venue_id = curve.venue_id();
  lvl.pair = curve.instrument().symbol();
  lvl.base_asset = curve.instrument().base();
  lvl.quote_asset = curve.instrument().quote();
  lvl.side = side;
  lvl.source_order_id =
      curve.venue_id() + "|" + lvl.pair + "|" + domain::ToString(side) + "|linear";
  lvl.price = Quantize(best_price, scale);             // anchor цена a
  lvl.quantity = Quantize(q_max, scale);
  lvl.remaining_quantity = lvl.quantity;
  // d_hl = |worst − best| = ценовой диапазон по всей глубине (= |b|·q_max ⇒ D=|b|).
  const double d_hl = std::fabs(worst_price - best_price);
  if (std::isfinite(d_hl) && d_hl > 0.0) {
    lvl.d_hl_override = Quantize(d_hl, scale);
  }
  lvl.ts_event_ms = TsToMs(curve.timestamp());
  out.push_back(std::move(lvl));
}

}  // namespace

std::vector<domain::ExternalOrderLevel> LinearSegmentsFromCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve, std::int32_t decimal_scale) {
  std::vector<domain::ExternalOrderLevel> levels;
  if (curve.has_bid_curve()) {
    AppendLinearSide(curve.bid_curve(), domain::LevelSide::kBid, curve, decimal_scale, levels);
  }
  if (curve.has_ask_curve()) {
    AppendLinearSide(curve.ask_curve(), domain::LevelSide::kAsk, curve, decimal_scale, levels);
  }
  return levels;
}

domain::VectorizeResult TwoSidedSegmentsFromCurves(
    const std::vector<fob::venue::v1::VenueLiquidityCurve>& curves,
    std::int32_t scale) {
  domain::VectorizeResult vr;
  // 1. Общий basis: отсортированное объединение base+quote всех пар (R-F05A-002).
  std::set<std::string> assets;
  for (const auto& c : curves) {
    if (!c.instrument().base().empty()) assets.insert(c.instrument().base());
    if (!c.instrument().quote().empty()) assets.insert(c.instrument().quote());
  }
  int idx = 0;
  for (const auto& a : assets) {  // std::set отсортирован
    vr.basis.assets.push_back(a);
    vr.basis.index_of.emplace(a, idx++);
  }
  vr.basis.num_assets = static_cast<int>(vr.basis.assets.size());
  const int N = vr.basis.num_assets;

  // 2. Один двусторонний сегмент на кривую (нужны обе стороны для mid).
  for (const auto& c : curves) {
    if (!c.has_bid_curve() || !c.has_ask_curve()) continue;
    const auto& bid = c.bid_curve();
    const auto& ask = c.ask_curve();
    const int nb = std::min(bid.q_grid_size(), bid.p_of_q_size());
    const int na = std::min(ask.q_grid_size(), ask.p_of_q_size());
    if (nb < 1 || na < 1) continue;
    const double fob_best_bid = bid.p_of_q(0);
    const double fob_best_ask = ask.p_of_q(0);
    const double fob_q_bid = bid.q_grid(nb - 1);
    const double fob_q_ask = ask.q_grid(na - 1);
    if (!(fob_best_bid > 0.0) || !(fob_best_ask > 0.0)) continue;  // log требует > 0
    if (!(fob_q_bid > 0.0) || !(fob_q_ask > 0.0)) continue;

    const std::string model =
        cex::common::Env::get_string("F05A_TRANSLATOR_MODEL", "safe_vwap");
    double theta = 0.60;
    try {
      const auto tsv = cex::common::Env::try_get_string("F05A_SAFE_SHARE");
      if (tsv && !tsv->empty()) theta = std::stod(*tsv);
    } catch (...) { theta = 0.60; }
    if (!(theta > 0.0) || !(theta <= 1.0)) theta = 0.60;

    // ADR-053: наклон/anchor safe-translator берём ИЗ КРИВОЙ (venues посчитал из
    // сырого стакана на стадии построения кривой). Клиринг потребляет КРИВУЮ, не
    // сырой стакан. Fallback: log_endpoint либо safe_vwap из FOB-кривой.
    double mid = 0.0, q_bid = fob_q_bid, q_ask = fob_q_ask, log_mid = 0.0;
    double m = 0.0, alpha_ext = 0.0, alpha_t = 0.0, beta_t = 0.0;
    std::string model_tag;

    if (model != "log_endpoint" && c.has_safe_translator() &&
        c.safe_translator().slope() > 0.0) {
      const auto& st = c.safe_translator();
      mid = static_cast<double>(cex::common::Decimal::from_proto(st.mid()));
      if (!(mid > 0.0)) mid = 0.5 * (fob_best_bid + fob_best_ask);
      log_mid = (st.anchor_log() != 0.0) ? st.anchor_log() : std::log(mid);
      m = st.slope();
      alpha_ext = st.alpha_ext();
      alpha_t = st.alpha_t();
      beta_t = st.beta_t();
      if (st.theta() > 0.0) theta = st.theta();
      const double sqb = static_cast<double>(cex::common::Decimal::from_proto(st.q_bid()));
      const double sqa = static_cast<double>(cex::common::Decimal::from_proto(st.q_ask()));
      if (sqb > 0.0) q_bid = sqb;
      if (sqa > 0.0) q_ask = sqa;
      model_tag = st.model().empty() ? "safe_vwap_raw" : st.model();
    } else if (model == "log_endpoint") {
      mid = 0.5 * (fob_best_bid + fob_best_ask);
      log_mid = std::log(mid);
      const double bid_slope =
          std::fabs(std::log(bid.p_of_q(nb - 1)) - std::log(fob_best_bid)) / fob_q_bid;
      const double ask_slope =
          std::fabs(std::log(ask.p_of_q(na - 1)) - std::log(fob_best_ask)) / fob_q_ask;
      m = 0.5 * (bid_slope + ask_slope);
      model_tag = "log_endpoint";
    } else {
      // Fallback safe_vwap из FOB-кривой (кривая без safe_translator).
      mid = 0.5 * (fob_best_bid + fob_best_ask);
      log_mid = std::log(mid);
      const double a_bid = SideAlphaExt(bid, mid);
      const double a_ask = SideAlphaExt(ask, mid);
      alpha_ext = std::min(a_bid, a_ask);
      if (std::isfinite(alpha_ext) && alpha_ext > 0.0) {
        alpha_t = theta * alpha_ext;
        m = mid / (10000.0 * alpha_t);
        beta_t = (mid * mid) / (10000.0 * alpha_t);
      }
      model_tag = "safe_vwap_fob";
    }
    if (!(mid > 0.0)) continue;
    if (!std::isfinite(m) || m <= 0.0) m = 1e-12;  // P SPD floor (лог-масштаб)

    const int ix = vr.basis.IndexOf(c.instrument().base());
    const int iy = vr.basis.IndexOf(c.instrument().quote());
    if (ix < 0 || iy < 0) continue;

    domain::VectorFlowSegment seg;
    seg.venue_id = c.venue_id();
    seg.pair = c.instrument().symbol();
    seg.side = domain::LevelSide::kBid;  // двусторонний; поле номинально
    seg.segment_id = c.venue_id() + "|" + seg.pair + "|two_sided";
    seg.source_order_id = seg.segment_id;
    seg.w.assign(static_cast<std::size_t>(N), 0.0);
    seg.w[static_cast<std::size_t>(ix)] = 1.0;   // e_base
    seg.w[static_cast<std::size_t>(iy)] = -1.0;  // − e_quote (чистое направление)
    seg.anchor = Quantize(log_mid, scale);       // ADR-053: якорь = log(mid)
    seg.slope = Quantize(m, scale);              // лог-наклон m
    seg.alpha_ext = Quantize(std::isfinite(alpha_ext) ? alpha_ext : 0.0, scale);
    seg.alpha_t = Quantize(alpha_t, scale);      // после θ-haircut
    seg.beta_t = Quantize(beta_t, scale);        // линейный β_T = mid·m
    seg.theta = Quantize(theta, scale);
    seg.translator_model = model_tag;            // safe_vwap_raw|safe_vwap_fob|log_endpoint
    seg.q_max = Quantize(q_ask, scale);          // x_max = +Q_ask
    seg.q_min = Quantize(-q_bid, scale);         // x_min = −Q_bid
    // q_rate — суммарная пропускная способность двусторонней кривой (обе стороны).
    seg.q_rate = Quantize(q_ask + q_bid, scale);
    seg.effective_price = Quantize(mid, scale);  // РЕАЛЬНЫЙ mid (для отображения)
    seg.source_timestamp_ms = TsToMs(c.timestamp());
    vr.segments.push_back(std::move(seg));
  }
  return vr;
}

std::vector<domain::ExternalOrderLevel> LevelsFromCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve, std::int32_t decimal_scale) {
  std::vector<domain::ExternalOrderLevel> levels;
  if (curve.has_bid_curve()) {
    AppendSide(curve.bid_curve(), domain::LevelSide::kBid, curve, decimal_scale, levels);
  }
  if (curve.has_ask_curve()) {
    AppendSide(curve.ask_curve(), domain::LevelSide::kAsk, curve, decimal_scale, levels);
  }
  return levels;
}

}  // namespace cex::market_data::app
