#include "domain/depth_window.hpp"

#include <algorithm>
#include <cmath>

namespace cex::venues::domain {

namespace {

bool ValidLevel(const DepthLevel& l) {
  return std::isfinite(l.price) && std::isfinite(l.qty) && l.price > 0.0 && l.qty > 0.0;
}

}  // namespace

double QBarFromInvariance(double volume_usd, double sigma_day,
                          const DepthWindowConfig& cfg, double* mu_out) {
  if (!std::isfinite(volume_usd) || !std::isfinite(sigma_day) || !(volume_usd > 0.0) ||
      !(sigma_day > 0.0) || !(cfg.w_star_usd > 0.0) || !(cfg.zeta > 0.0)) {
    return 0.0;
  }
  const double w = sigma_day * volume_usd;
  const double mu = cfg.mu_star * std::pow(w / cfg.w_star_usd, cfg.mu_exponent);
  if (!std::isfinite(mu) || !(mu > 0.0)) return 0.0;
  if (mu_out != nullptr) *mu_out = mu;
  return volume_usd / (cfg.zeta * mu);
}

std::size_t LevelsToReachVolume(const std::vector<DepthLevel>& levels, double v_cal,
                                bool* reached) {
  double cum = 0.0;
  for (std::size_t i = 0; i < levels.size(); ++i) {
    if (!ValidLevel(levels[i])) continue;
    cum += levels[i].price * levels[i].qty;
    if (cum >= v_cal) {
      if (reached != nullptr) *reached = true;
      return i + 1;
    }
  }
  if (reached != nullptr) *reached = false;
  return levels.size();
}

bool SlippageBpsAtVolume(const std::vector<DepthLevel>& levels, double mid, double v_cal,
                         double* delta_bps) {
  if (!(mid > 0.0) || !(v_cal > 0.0) || !std::isfinite(mid) || !std::isfinite(v_cal)) {
    return false;
  }
  double cum_notional = 0.0;
  double cum_qty = 0.0;
  for (const auto& l : levels) {
    if (!ValidLevel(l)) continue;
    const double level_notional = l.price * l.qty;
    if (cum_notional + level_notional >= v_cal) {
      // Последний уровень берём частично — ровно до V_cal.
      cum_qty += (v_cal - cum_notional) / l.price;
      const double vwap = v_cal / cum_qty;
      if (!(vwap > 0.0)) return false;
      if (delta_bps != nullptr) *delta_bps = 1.0e4 * std::fabs(std::log(vwap / mid));
      return true;
    }
    cum_notional += level_notional;
    cum_qty += l.qty;
  }
  return false;
}

DepthWindowResult ComputeDepthWindow(const DepthWindowConfig& cfg,
                                     const DepthWindowInputs& in,
                                     const std::vector<DepthLevel>& bids,
                                     const std::vector<DepthLevel>& asks, double mid,
                                     DepthWindowState* state) {
  DepthWindowResult r;

  double q_bar = 0.0;
  double mu = 0.0;
  if (in.q_bar_override_usd > 0.0 && std::isfinite(in.q_bar_override_usd)) {
    q_bar = in.q_bar_override_usd;
  } else {
    q_bar = QBarFromInvariance(in.volume_usd_24h, in.sigma_day, cfg, &mu);
  }
  if (!(q_bar > 0.0) || !(cfg.k > 0.0) || !(mid > 0.0) || !std::isfinite(mid) ||
      bids.empty() || asks.empty()) {
    r.no_inputs = true;  // окно не применяется — вызывающий оставляет depth_levels
    return r;
  }

  r.q_bar_usd = q_bar;
  r.mu_per_day = mu;
  r.v_cal_raw_usd = cfg.k * q_bar;
  r.v_cal_usd = r.v_cal_raw_usd;
  if (state != nullptr && state->has_prev && state->v_cal_ewma > 0.0) {
    const double a = std::clamp(cfg.ewma_alpha, 0.0, 1.0);
    r.v_cal_usd = a * r.v_cal_raw_usd + (1.0 - a) * state->v_cal_ewma;
  }

  bool reached_bid = false;
  bool reached_ask = false;
  r.n_bid = LevelsToReachVolume(bids, r.v_cal_usd, &reached_bid);
  r.n_ask = LevelsToReachVolume(asks, r.v_cal_usd, &reached_ask);
  r.bid_reached = reached_bid;
  r.ask_reached = reached_ask;
  r.truncated = !reached_bid || !reached_ask;

  std::size_t raw_n = std::max(r.n_bid, r.n_ask);
  const std::size_t cap = std::max<std::size_t>(1, cfg.max_levels);
  bool cut_by_cap = false;
  if (raw_n > cap) {
    raw_n = cap;
    cut_by_cap = true;
  }
  // «Упёрлись в потолок API»: окно не набрано, а видимая сторона уже достигла потолка.
  const bool bid_at_cap = !reached_bid && bids.size() >= cap;
  const bool ask_at_cap = !reached_ask && asks.size() >= cap;
  r.api_capped = cut_by_cap || bid_at_cap || ask_at_cap;

  std::size_t n = raw_n;
  if (state != nullptr && state->has_prev && state->n_prev > 0) {
    const double prev = static_cast<double>(state->n_prev);
    const double lo = prev * (1.0 - cfg.hysteresis);
    const double hi = prev * (1.0 + cfg.hysteresis);
    const double cand = static_cast<double>(raw_n);
    if (cand >= lo && cand <= hi) n = state->n_prev;
  }
  n = std::clamp<std::size_t>(n, 1, cap);
  r.n = n;
  r.coarse_tick = !r.truncated && n < cfg.min_levels;

  double d_bid = 0.0;
  double d_ask = 0.0;
  const bool have_bid = SlippageBpsAtVolume(bids, mid, r.v_cal_usd, &d_bid);
  const bool have_ask = SlippageBpsAtVolume(asks, mid, r.v_cal_usd, &d_ask);
  r.delta_bid_bps = have_bid ? d_bid : 0.0;
  r.delta_ask_bps = have_ask ? d_ask : 0.0;
  if (have_bid && have_ask) {
    r.delta_n_bps = 0.5 * (d_bid + d_ask);
  } else if (have_bid) {
    r.delta_n_bps = d_bid;
  } else if (have_ask) {
    r.delta_n_bps = d_ask;
  }
  if (r.delta_n_bps > 1.0e-9) r.alpha_chord = r.v_cal_usd / r.delta_n_bps;

  r.applied = true;
  if (state != nullptr) {
    state->has_prev = true;
    state->v_cal_ewma = r.v_cal_usd;
    state->n_prev = n;
  }
  return r;
}

std::size_t NextRequestLimit(std::size_t n_prev, std::size_t api_cap, double headroom,
                             const std::vector<std::size_t>& ladder,
                             std::size_t min_limit) {
  const std::size_t cap = std::max<std::size_t>(1, api_cap);
  if (n_prev == 0) return cap;  // первый запрос — на потолке API
  const double h = std::isfinite(headroom) && headroom >= 1.0 ? headroom : 1.0;
  std::size_t want =
      static_cast<std::size_t>(std::ceil(static_cast<double>(n_prev) * h));
  want = std::max(want, std::min(min_limit, cap));
  if (!ladder.empty()) {
    for (const std::size_t tier : ladder) {
      if (tier >= want) return std::min(tier, cap);
    }
    return cap;
  }
  return std::min(want, cap);
}

}  // namespace cex::venues::domain
