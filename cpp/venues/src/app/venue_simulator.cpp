#include "app/venue_simulator.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace cex::venues::app {

namespace {

using fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_UNDERFILLED;

double Dec2D(const fob::common::v1::Decimal& d) {
  return static_cast<double>(d.units()) * std::pow(10.0, -d.scale());
}

double Dec2D(const cex::common::Decimal& d) {
  return static_cast<double>(d);
}

// Convert a double to Decimal{units, scale} at a fixed scale.
cex::common::Decimal D2Dec(double value, int32_t scale = 8) {
  if (!std::isfinite(value)) return cex::common::Decimal::zero();
  double scaled = value;
  for (int32_t i = 0; i < scale; ++i) scaled *= 10.0;
  return cex::common::Decimal{static_cast<int64_t>(std::llround(scaled)), scale};
}

struct Level {
  double price;
  double qty;
};

// Extract the side of the book the order will hit:
//   BUY  -> asks, ascending price (cheapest first)
//   SELL -> bids, descending price (highest first)
std::vector<Level> ExtractLevels(const fob::venue::v1::VenueSnapshot& s,
                                 fob::common::v1::Side side) {
  std::vector<Level> levels;
  if (side == fob::common::v1::SIDE_BUY) {
    const int n = std::min(s.ask_prices_size(), s.ask_quantities_size());
    levels.reserve(n);
    for (int i = 0; i < n; ++i) {
      levels.push_back({Dec2D(s.ask_prices(i)), Dec2D(s.ask_quantities(i))});
    }
    std::sort(levels.begin(), levels.end(),
              [](const Level& a, const Level& b) { return a.price < b.price; });
  } else {
    const int n = std::min(s.bid_prices_size(), s.bid_quantities_size());
    levels.reserve(n);
    for (int i = 0; i < n; ++i) {
      levels.push_back({Dec2D(s.bid_prices(i)), Dec2D(s.bid_quantities(i))});
    }
    std::sort(levels.begin(), levels.end(),
              [](const Level& a, const Level& b) { return a.price > b.price; });
  }
  return levels;
}

double MidPrice(const fob::venue::v1::VenueSnapshot& s) {
  if (s.has_mid_price() && Dec2D(s.mid_price()) > 0.0) return Dec2D(s.mid_price());
  const double bb = s.has_best_bid() ? Dec2D(s.best_bid()) : 0.0;
  const double ba = s.has_best_ask() ? Dec2D(s.best_ask()) : 0.0;
  if (bb > 0.0 && ba > 0.0) return 0.5 * (bb + ba);
  return bb > 0.0 ? bb : ba;
}

// ImpactModel delta on top of the book-walk VWAP. LEVEL_BY_LEVEL means the
// walk itself is the impact, so the parametric delta is zero.
double ImpactDelta(const fob::sim::v1::ImpactModel& m, double filled_qty,
                   double mid) {
  const double a = m.impact_coeff();
  switch (m.model_type()) {
    case fob::sim::v1::IMPACT_MODEL_TYPE_LINEAR:
      return a * filled_qty;
    case fob::sim::v1::IMPACT_MODEL_TYPE_SQRT:
      return a * std::sqrt(std::max(0.0, filled_qty));
    case fob::sim::v1::IMPACT_MODEL_TYPE_POWER_LAW:
      return a * std::pow(std::max(0.0, filled_qty),
                          m.power_exponent() > 0 ? m.power_exponent() : 1.0);
    case fob::sim::v1::IMPACT_MODEL_TYPE_LEVEL_BY_LEVEL:
    default:
      (void)mid;
      return 0.0;
  }
}

double FeeRate(const fob::sim::v1::FeeModel& m,
               fob::execution::v1::ExecutionStrategy order_type) {
  // taker for MARKET/IOC-ish, maker for LIMIT/POST_ONLY.
  const bool maker = order_type == fob::execution::v1::EXEC_STRATEGY_LIMIT ||
                     order_type == fob::execution::v1::EXEC_STRATEGY_POST_ONLY;
  const int32_t bps = maker ? m.maker_bps() : m.taker_bps();
  return static_cast<double>(bps) / 10000.0;
}

}  // namespace

SimulateResult VenueSimulator::Simulate(const SimulateRequest& request,
                                        const SimModels& models) const {
  SimulateResult out;
  if (request.snapshot != nullptr) {
    out.lob_snapshot_id = request.snapshot->meta().event_id();
  }
  out.remaining_qty = request.target_qty;

  std::mt19937_64 rng(request.rng_seed);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  // --- Check: stale LOB -------------------------------------------------
  if (request.lob_age_ms > request.stale_threshold_ms) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_STALE_LOB";
    return out;
  }
  if (request.snapshot == nullptr) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_NO_LIQUIDITY";
    return out;
  }

  // --- Check: rate-limit random reject (before doing work) --------------
  if (models.rejection.rate_limit_reject_rate() > 0.0 &&
      unit(rng) < models.rejection.rate_limit_reject_rate()) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_RATE_LIMIT";
    return out;
  }

  const double target = Dec2D(request.target_qty);
  const double mid = MidPrice(*request.snapshot);
  const bool is_buy = request.side == fob::common::v1::SIDE_BUY;
  const double limit = Dec2D(request.limit_price);
  const bool has_limit =
      request.has_limit_price &&
      request.order_type != fob::execution::v1::EXEC_STRATEGY_MARKET;

  // --- LEVEL_BY_LEVEL matching -----------------------------------------
  auto levels = ExtractLevels(*request.snapshot, request.side);
  double filled = 0.0;
  double notional = 0.0;  // sum(price*qty) for VWAP
  for (const auto& lvl : levels) {
    if (filled >= target) break;
    if (lvl.qty <= 0.0 || lvl.price <= 0.0) continue;
    // LIMIT: skip levels worse than the limit price.
    if (has_limit) {
      if (is_buy && lvl.price > limit) continue;
      if (!is_buy && lvl.price < limit) continue;
    }
    const double take = std::min(lvl.qty, target - filled);
    filled += take;
    notional += take * lvl.price;
  }

  // --- Price constraint reject -----------------------------------------
  if (models.rejection.price_constraint_enabled() && has_limit && filled <= 0.0) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_PRICE_CONSTRAINT";
    return out;
  }

  // --- No liquidity -----------------------------------------------------
  if (filled <= 0.0) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_NO_LIQUIDITY";
    return out;
  }

  // --- Min-liquidity threshold reject ----------------------------------
  const double min_liq = Dec2D(models.rejection.min_liquidity_threshold());
  if (models.rejection.insufficient_liquidity_enabled() && min_liq > 0.0 &&
      filled < min_liq) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_NO_LIQUIDITY";
    return out;
  }

  // --- Random reject ----------------------------------------------------
  if (models.rejection.random_rejection_rate() > 0.0 &&
      unit(rng) < models.rejection.random_rejection_rate()) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_RANDOM_REJECT";
    return out;
  }

  double vwap = notional / filled;

  // --- Impact model -----------------------------------------------------
  const double delta = ImpactDelta(models.impact, filled, mid);
  const double avg_with_impact = is_buy ? vwap + delta : vwap - delta;
  if (mid > 0.0) {
    out.impact_bps = std::fabs(avg_with_impact - mid) / mid * 10000.0;
    out.slippage_bps = static_cast<int32_t>(std::llround(
        (avg_with_impact - mid) / mid * 10000.0 * (is_buy ? 1.0 : -1.0)));
  }

  // --- Fee model --------------------------------------------------------
  const double fee_rate = FeeRate(models.fee, request.order_type);
  double fee = filled * avg_with_impact * fee_rate;
  const double min_fee = Dec2D(models.fee.min_fee());
  if (min_fee > 0.0 && fee < min_fee) fee = min_fee;
  const double gas = Dec2D(models.fee.gas_flat());
  if (gas > 0.0) fee += gas;

  // --- Latency model ----------------------------------------------------
  uint32_t latency_ms = models.latency.p50_ms();
  switch (models.latency.distribution()) {
    case fob::sim::v1::LATENCY_DISTRIBUTION_FIXED:
      latency_ms = models.latency.p50_ms();
      break;
    case fob::sim::v1::LATENCY_DISTRIBUTION_UNIFORM: {
      const double lo = models.latency.p50_ms();
      const double hi = std::max<double>(lo, models.latency.p99_ms());
      latency_ms = static_cast<uint32_t>(lo + unit(rng) * (hi - lo));
      break;
    }
    case fob::sim::v1::LATENCY_DISTRIBUTION_LOGNORMAL:
    case fob::sim::v1::LATENCY_DISTRIBUTION_EMPIRICAL:
    default: {
      // Approximate lognormal from p50/p95: median = p50, shape from p95.
      const double p50 = std::max<double>(1.0, models.latency.p50_ms());
      const double p95 = std::max<double>(p50, models.latency.p95_ms());
      const double mu = std::log(p50);
      const double sigma = std::max(0.0, (std::log(p95) - mu) / 1.645);
      std::lognormal_distribution<double> ln(mu, sigma);
      // tail probability: occasionally inflate.
      double sample = ln(rng);
      if (models.latency.tail_probability() > 0.0 &&
          unit(rng) < models.latency.tail_probability()) {
        sample *= 3.0;
      }
      latency_ms = static_cast<uint32_t>(std::max(0.0, sample));
      break;
    }
  }
  out.latency_sample_ms = latency_ms;

  // Latency timeout -> reject (mimics venue hang).
  if (models.latency.timeout_ms() > 0 && latency_ms > models.latency.timeout_ms()) {
    out.status = EXECUTION_REPORT_STATUS_REJECTED;
    out.reject_reason = "SIM_TIMEOUT";
    // keep latency_sample_ms set for diagnostics
    out.filled_qty = cex::common::Decimal::zero();
    out.remaining_qty = request.target_qty;
    return out;
  }

  // --- Build successful result -----------------------------------------
  out.filled_qty = D2Dec(filled);
  out.remaining_qty = D2Dec(std::max(0.0, target - filled));
  out.avg_price = D2Dec(avg_with_impact);
  out.fee = D2Dec(fee);
  if (request.snapshot->has_taker_fee() || request.snapshot->has_maker_fee()) {
    out.fee_currency = request.symbol;  // placeholder: quote-ccy resolution is venue-specific
  }

  const double eps = 1e-9;
  if (filled + eps >= target) {
    out.status = EXECUTION_REPORT_STATUS_FILLED;
  } else if (request.partial_fill_mode == fob::sim::v1::PARTIAL_FILL_MODE_NONE) {
    // partial not allowed -> treat as underfilled (caller may retry/fallback)
    out.status = EXECUTION_REPORT_STATUS_UNDERFILLED;
  } else {
    out.status = EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  return out;
}

}  // namespace cex::venues::app
