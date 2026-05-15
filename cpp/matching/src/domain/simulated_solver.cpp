#include "domain/simulated_solver.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cex/common/decimal.hpp"
#include "cex/common/time.hpp"

namespace cex::matching::domain {

using cex::common::Decimal;

namespace {

struct PlannedOrder {
  const fob::orders::v1::FlowOrder* order{nullptr};
  Decimal requested{};
  Decimal internal_filled{};
  Decimal external_filled{};
};

struct SymbolPlan {
  std::vector<PlannedOrder> buys;
  std::vector<PlannedOrder> sells;
};

int64_t Pow10(const int32_t power) {
  int64_t out = 1;
  for (int32_t i = 0; i < power; ++i) out *= 10;
  return out;
}

Decimal AlignTo(const Decimal& value, const int32_t scale) {
  Decimal out = value;
  if (out.scale == scale) return out;
  if (out.scale < scale) {
    out.units *= Pow10(scale - out.scale);
    out.scale = scale;
    return out;
  }
  out.units /= Pow10(out.scale - scale);
  out.scale = scale;
  return out;
}

Decimal FromDouble(const double value, const int32_t scale) {
  const double mul = std::pow(10.0, static_cast<double>(scale));
  return Decimal{
      .units = static_cast<int64_t>(std::llround(value * mul)),
      .scale = scale,
  };
}

Decimal Midpoint(const Decimal& a, const Decimal& b) {
  Decimal sum = Decimal::add(a, b);
  sum.units /= 2;
  return sum;
}

Decimal SelectInternalPrice(
    const fob::orders::v1::FlowOrder& order,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices) {
  const Decimal price_low = Decimal::from_proto(order.price_low());
  const Decimal price_high = Decimal::from_proto(order.price_high());
  const Decimal midpoint = Midpoint(price_low, price_high);

  const auto it = reference_prices.find(order.instrument().symbol());
  if (it == reference_prices.end()) {
    return midpoint;
  }

  const Decimal ref_price = Decimal::from_proto(it->second);
  if (Decimal::cmp(ref_price, price_low) >= 0 &&
      Decimal::cmp(ref_price, price_high) <= 0) {
    return ref_price;
  }
  return midpoint;
}

Decimal MulByMs(const Decimal& per_sec, const int ms) {
  Decimal out = per_sec;
  __int128 units = static_cast<__int128>(out.units) * static_cast<__int128>(ms);
  units /= 1000;
  out.units = static_cast<int64_t>(units);
  return out;
}

Decimal RateFromQty(const Decimal& qty, const int ms) {
  Decimal out = qty;
  if (ms <= 0) return out;
  __int128 units = static_cast<__int128>(out.units) * 1000;
  units /= ms;
  out.units = static_cast<int64_t>(units);
  return out;
}

Decimal MulRatio(const Decimal& value, const Decimal& part, const Decimal& whole) {
  const int32_t scale = std::max(value.scale, std::max(part.scale, whole.scale));
  const Decimal v = AlignTo(value, scale);
  const Decimal p = AlignTo(part, scale);
  const Decimal w = AlignTo(whole, scale);
  if (w.units == 0) {
    return Decimal{0, scale};
  }
  __int128 units = static_cast<__int128>(v.units) * static_cast<__int128>(p.units);
  units /= static_cast<__int128>(w.units);
  return Decimal{
      .units = static_cast<int64_t>(units),
      .scale = scale,
  };
}

Decimal TotalFilled(const PlannedOrder& order) {
  return Decimal::add(order.internal_filled, order.external_filled);
}

Decimal ExternalNeed(const PlannedOrder& order) {
  return Decimal::sub(order.requested, order.internal_filled);
}

template <class GetNeed, class AddFill>
void AllocateProRata(std::vector<PlannedOrder*> orders,
                     const Decimal& total_to_allocate,
                     GetNeed get_need,
                     AddFill add_fill) {
  std::vector<PlannedOrder*> active;
  active.reserve(orders.size());
  Decimal total_need = Decimal::zero();

  for (PlannedOrder* order : orders) {
    const Decimal need = get_need(*order);
    if (need.units <= 0) continue;
    active.push_back(order);
    total_need = Decimal::add(total_need, need);
  }

  if (active.empty() || total_to_allocate.units <= 0) {
    return;
  }

  Decimal left = total_to_allocate;
  for (size_t i = 0; i < active.size(); ++i) {
    PlannedOrder* order = active[i];
    const Decimal need = get_need(*order);
    Decimal share = need;
    if (i + 1 != active.size()) {
      share = MulRatio(need, left, total_need);
      share = Decimal::min(share, need);
    } else {
      share = Decimal::min(left, need);
    }
    if (share.units <= 0) continue;
    add_fill(*order, share);
    left = Decimal::sub(left, share);
    total_need = Decimal::sub(total_need, need);
  }
}

std::string LiquiditySourceForVenue(const std::string& venue_id) {
  if (venue_id.find("uniswap") != std::string::npos ||
      venue_id.find("dex") != std::string::npos) {
    return "dex_hedge";
  }
  return "cex_hedge";
}

std::string CurveIdForVenueCurve(const fob::venue::v1::VenueLiquidityCurve& curve) {
  if (!curve.curve_id().empty()) {
    return curve.curve_id();
  }
  if (curve.has_meta() && !curve.meta().event_id().empty()) {
    return curve.meta().event_id();
  }
  return "";
}

const fob::venue::v1::SideLiquidityCurve* SelectExternalCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve,
    const fob::common::v1::Side side) {
  if (side == fob::common::v1::SIDE_BUY) {
    return curve.has_ask_curve() ? &curve.ask_curve() : nullptr;
  }
  if (side == fob::common::v1::SIDE_SELL) {
    return curve.has_bid_curve() ? &curve.bid_curve() : nullptr;
  }
  return nullptr;
}

Decimal ExternalAvailableQty(const fob::venue::v1::VenueLiquidityCurve& curve,
                             const fob::common::v1::Side side,
                             const int32_t qty_scale) {
  const auto* side_curve = SelectExternalCurve(curve, side);
  if (side_curve == nullptr || side_curve->q_grid().empty()) {
    return Decimal{0, qty_scale};
  }
  return FromDouble(side_curve->q_grid(side_curve->q_grid_size() - 1), qty_scale);
}

std::optional<Decimal> ExternalPriceAtQty(const fob::venue::v1::VenueLiquidityCurve& curve,
                                          const fob::common::v1::Side side,
                                          const Decimal& qty) {
  const auto* side_curve = SelectExternalCurve(curve, side);
  if (side_curve == nullptr || side_curve->q_grid().empty() || side_curve->p_of_q().empty()) {
    return std::nullopt;
  }

  size_t idx = side_curve->q_grid_size() - 1;
  const double target = static_cast<double>(qty);
  for (size_t i = 0; i < static_cast<size_t>(side_curve->q_grid_size()); ++i) {
    if (side_curve->q_grid(static_cast<int>(i)) >= target) {
      idx = i;
      break;
    }
  }

  if (idx >= static_cast<size_t>(side_curve->p_of_q_size())) {
    idx = static_cast<size_t>(side_curve->p_of_q_size() - 1);
  }

  const int32_t price_scale = curve.has_mid_price() ? curve.mid_price().scale() : 0;
  return FromDouble(side_curve->p_of_q(static_cast<int>(idx)), price_scale);
}

void AddFill(fob::matching::v1::BatchResult* batch,
             const fob::orders::v1::FlowOrder& order,
             const Decimal& qty,
             const Decimal& price,
             const std::string& liquidity_source,
             const fob::venue::v1::VenueLiquidityCurve* curve = nullptr) {
  if (qty.units <= 0) return;

  auto* fill = batch->add_fills();
  fill->set_order_id(order.order_id());
  fill->set_user_id(order.user_id());
  *fill->mutable_instrument() = order.instrument();
  fill->set_side(order.side());
  *fill->mutable_executed_qty() = qty.to_proto();
  *fill->mutable_price() = price.to_proto();
  *fill->mutable_executed_notional() = Decimal::mul(qty, price).to_proto();
  fill->set_liquidity_source(liquidity_source);
  auto* provenance = fill->mutable_provenance();
  provenance->set_liquidity_source(liquidity_source);
  if (curve != nullptr) {
    provenance->set_venue_id(curve->venue_id());
    provenance->set_snapshot_id(curve->snapshot_id());
    provenance->set_curve_id(CurveIdForVenueCurve(*curve));
  }
}

Decimal ClearPriceFromTotals(const Decimal& notional, const Decimal& qty) {
  if (qty.units == 0) {
    return Decimal::zero();
  }
  const int32_t scale = std::max(0, notional.scale - qty.scale);
  const double value = static_cast<double>(notional) / static_cast<double>(qty);
  return FromDouble(value, scale);
}

void AddOrderUpdate(fob::matching::v1::BatchResult* batch,
                    const fob::orders::v1::FlowOrder& order,
                    const Decimal& total_filled,
                    const google::protobuf::Timestamp& now) {
  if (total_filled.units <= 0) return;

  Decimal remaining = Decimal::from_proto(order.remaining_qty());
  remaining = Decimal::sub(remaining, total_filled);
  if (remaining.units < 0) {
    remaining.units = 0;
  }

  auto* update = batch->add_order_updates();
  update->set_order_id(order.order_id());
  update->set_status(remaining.units == 0
                         ? fob::common::v1::ORDER_STATUS_FILLED
                         : fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
  *update->mutable_remaining_qty() = remaining.to_proto();
  *update->mutable_filled_qty_total() =
      Decimal::sub(Decimal::from_proto(order.total_qty()), remaining).to_proto();
  *update->mutable_updated_at() = now;
}

}  // namespace

SimulatedContinuousClearingSolver::SimulatedContinuousClearingSolver(
    const int batch_interval_ms)
    : batch_interval_ms_(batch_interval_ms) {}

void SimulatedContinuousClearingSolver::set_batch_interval_ms(const int batch_interval_ms) {
  batch_interval_ms_ = batch_interval_ms;
}

fob::matching::v1::BatchResult SimulatedContinuousClearingSolver::Solve(
    const std::vector<fob::orders::v1::FlowOrder>& active_orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
    const ExternalLiquidityBySymbol& external_liquidity) {
  fob::matching::v1::BatchResult batch;

  auto* diag = batch.mutable_diagnostics();
  diag->set_residual_norm(0.0);
  diag->set_solve_time_ms(1);
  diag->set_num_active_orders(static_cast<uint32_t>(active_orders.size()));
  diag->set_config_version(1);

  std::unordered_map<std::string, SymbolPlan> by_symbol;
  for (const auto& order : active_orders) {
    Decimal remaining = Decimal::from_proto(order.remaining_qty());
    Decimal speed = Decimal::from_proto(order.max_speed());
    Decimal requested = MulByMs(speed, batch_interval_ms_);
    if (Decimal::cmp(requested, remaining) > 0) {
      requested = remaining;
    }
    if (requested.units <= 0) continue;

    PlannedOrder planned;
    planned.order = &order;
    planned.requested = requested;
    planned.internal_filled = Decimal{0, requested.scale};
    planned.external_filled = Decimal{0, requested.scale};

    auto& plan = by_symbol[order.instrument().symbol()];
    if (order.side() == fob::common::v1::SIDE_BUY) {
      plan.buys.push_back(planned);
    } else if (order.side() == fob::common::v1::SIDE_SELL) {
      plan.sells.push_back(planned);
    }
  }

  const auto now = cex::common::now_ts();
  int external_symbols_used = 0;
  double residual_norm = 0.0;

  for (auto& [symbol, plan] : by_symbol) {
    Decimal total_buy = Decimal::zero();
    for (const auto& order : plan.buys) total_buy = Decimal::add(total_buy, order.requested);

    Decimal total_sell = Decimal::zero();
    for (const auto& order : plan.sells) total_sell = Decimal::add(total_sell, order.requested);

    const Decimal internal_total = Decimal::min(total_buy, total_sell);

    std::vector<PlannedOrder*> buy_ptrs;
    buy_ptrs.reserve(plan.buys.size());
    for (auto& order : plan.buys) buy_ptrs.push_back(&order);
    AllocateProRata(
        buy_ptrs, internal_total,
        [](const PlannedOrder& order) { return order.requested; },
        [](PlannedOrder& order, const Decimal& fill) {
          order.internal_filled = Decimal::add(order.internal_filled, fill);
        });

    std::vector<PlannedOrder*> sell_ptrs;
    sell_ptrs.reserve(plan.sells.size());
    for (auto& order : plan.sells) sell_ptrs.push_back(&order);
    AllocateProRata(
        sell_ptrs, internal_total,
        [](const PlannedOrder& order) { return order.requested; },
        [](PlannedOrder& order, const Decimal& fill) {
          order.internal_filled = Decimal::add(order.internal_filled, fill);
        });

    fob::common::v1::Side residual_side = fob::common::v1::SIDE_UNSPECIFIED;
    Decimal residual_qty = Decimal::zero();
    std::vector<PlannedOrder*> residual_orders;

    if (Decimal::cmp(total_buy, total_sell) > 0) {
      residual_side = fob::common::v1::SIDE_BUY;
      residual_qty = Decimal::sub(total_buy, total_sell);
      for (auto& order : plan.buys) residual_orders.push_back(&order);
    } else if (Decimal::cmp(total_sell, total_buy) > 0) {
      residual_side = fob::common::v1::SIDE_SELL;
      residual_qty = Decimal::sub(total_sell, total_buy);
      for (auto& order : plan.sells) residual_orders.push_back(&order);
    }

    Decimal external_total{0, residual_qty.scale};
    const auto curve_it = external_liquidity.find(symbol);
    if (curve_it != external_liquidity.end() &&
        residual_side != fob::common::v1::SIDE_UNSPECIFIED) {
      const Decimal available = ExternalAvailableQty(
          curve_it->second, residual_side, residual_qty.scale);
      external_total = Decimal::min(residual_qty, available);
      if (external_total.units > 0) {
        ++external_symbols_used;
      }
    }

    AllocateProRata(
        residual_orders, external_total,
        [](const PlannedOrder& order) { return ExternalNeed(order); },
        [](PlannedOrder& order, const Decimal& fill) {
          order.external_filled = Decimal::add(order.external_filled, fill);
        });

    Decimal total_symbol_qty = Decimal::zero();
    Decimal total_symbol_notional = Decimal::zero();
    Decimal external_consumed{0, residual_qty.scale};

    for (const auto& order : plan.buys) {
      if (order.internal_filled.units > 0) {
        const Decimal price = SelectInternalPrice(*order.order, reference_prices);
        AddFill(&batch, *order.order, order.internal_filled, price, "internal");
        total_symbol_qty = Decimal::add(total_symbol_qty, order.internal_filled);
        total_symbol_notional = Decimal::add(
            total_symbol_notional, Decimal::mul(order.internal_filled, price));
      }
      if (order.external_filled.units > 0 && curve_it != external_liquidity.end()) {
        external_consumed = Decimal::add(external_consumed, order.external_filled);
        const auto maybe_price = ExternalPriceAtQty(
            curve_it->second, order.order->side(), external_consumed);
        if (maybe_price.has_value()) {
          AddFill(&batch, *order.order, order.external_filled, *maybe_price,
                  LiquiditySourceForVenue(curve_it->second.venue_id()),
                  &curve_it->second);
          total_symbol_qty = Decimal::add(total_symbol_qty, order.external_filled);
          total_symbol_notional = Decimal::add(
              total_symbol_notional, Decimal::mul(order.external_filled, *maybe_price));
        }
      }

      const Decimal total_filled = TotalFilled(order);
      if (total_filled.units > 0) {
        (*batch.mutable_executed_rates())[order.order->order_id()] =
            RateFromQty(total_filled, batch_interval_ms_).to_proto();
        AddOrderUpdate(&batch, *order.order, total_filled, now);
      }
    }

    for (const auto& order : plan.sells) {
      if (order.internal_filled.units > 0) {
        const Decimal price = SelectInternalPrice(*order.order, reference_prices);
        AddFill(&batch, *order.order, order.internal_filled, price, "internal");
        total_symbol_qty = Decimal::add(total_symbol_qty, order.internal_filled);
        total_symbol_notional = Decimal::add(
            total_symbol_notional, Decimal::mul(order.internal_filled, price));
      }
      if (order.external_filled.units > 0 && curve_it != external_liquidity.end()) {
        external_consumed = Decimal::add(external_consumed, order.external_filled);
        const auto maybe_price = ExternalPriceAtQty(
            curve_it->second, order.order->side(), external_consumed);
        if (maybe_price.has_value()) {
          AddFill(&batch, *order.order, order.external_filled, *maybe_price,
                  LiquiditySourceForVenue(curve_it->second.venue_id()),
                  &curve_it->second);
          total_symbol_qty = Decimal::add(total_symbol_qty, order.external_filled);
          total_symbol_notional = Decimal::add(
              total_symbol_notional, Decimal::mul(order.external_filled, *maybe_price));
        }
      }

      const Decimal total_filled = TotalFilled(order);
      if (total_filled.units > 0) {
        (*batch.mutable_executed_rates())[order.order->order_id()] =
            RateFromQty(total_filled, batch_interval_ms_).to_proto();
        AddOrderUpdate(&batch, *order.order, total_filled, now);
      }
    }

    if (total_symbol_qty.units > 0) {
      (*batch.mutable_clear_prices())[symbol] =
          ClearPriceFromTotals(total_symbol_notional, total_symbol_qty).to_proto();
    }

    const Decimal unresolved = Decimal::sub(residual_qty, external_total);
    if (unresolved.units > 0) {
      residual_norm += std::abs(static_cast<double>(unresolved));
    }
  }

  diag->set_residual_norm(residual_norm);
  diag->set_solver_diagnostics_json(
      "{\"kind\":\"mvp_simulator\",\"iterations\":1,\"external_symbols_used\":" +
      std::to_string(external_symbols_used) + "}");

  return batch;
}

void SimulatedContinuousClearingSolver::SetBatchIntervalMs(const int batch_interval_ms) {
  batch_interval_ms_ = batch_interval_ms;
}

}  // namespace cex::matching::domain
