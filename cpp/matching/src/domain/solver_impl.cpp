#include "domain/solver_impl.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"
#include "cex/common/time.hpp"

namespace cex::matching::domain {

namespace {

constexpr double kQtyEps = 1e-12;

struct PlannedLegFill {
    const FlowOrder* order{nullptr};
    size_t order_index{0};
    std::string symbol;
    fob::common::v1::Side side{fob::common::v1::SIDE_UNSPECIFIED};
    double order_qty{0.0};
    double max_leg_qty{0.0};
    double weight_abs{0.0};
    double requested_leg_qty{0.0};
    double clear_price{0.0};
    double actual_leg_qty{0.0};
    double internal_leg_qty{0.0};
    double external_leg_qty{0.0};
};

struct SymbolPlan {
    std::vector<PlannedLegFill*> buys;
    std::vector<PlannedLegFill*> sells;
    bool all_single_leg{true};
};

static cex::common::Decimal ToDecimal(double dbl) {
    return {static_cast<int64_t>(std::llround(dbl * 1e12)), 12};
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
    const fob::common::v1::Side side
) {
    if (side == fob::common::v1::SIDE_BUY) {
        return curve.has_ask_curve() ? &curve.ask_curve() : nullptr;
    }
    if (side == fob::common::v1::SIDE_SELL) {
        return curve.has_bid_curve() ? &curve.bid_curve() : nullptr;
    }
    return nullptr;
}

std::optional<double> ExternalPriceAtQty(const fob::venue::v1::VenueLiquidityCurve& curve,
                                         const fob::common::v1::Side side,
                                         const double qty) {
    const auto* side_curve = SelectExternalCurve(curve, side);
    if (side_curve == nullptr || side_curve->q_grid().empty() || side_curve->p_of_q().empty()) {
        return std::nullopt;
    }

    size_t idx = side_curve->q_grid_size() - 1;
    for (size_t i = 0; i < static_cast<size_t>(side_curve->q_grid_size()); ++i) {
        if (side_curve->q_grid(static_cast<int>(i)) + kQtyEps >= qty) {
            idx = i;
            break;
        }
    }

    if (idx >= static_cast<size_t>(side_curve->p_of_q_size())) {
        idx = static_cast<size_t>(side_curve->p_of_q_size() - 1);
    }

    return side_curve->p_of_q(static_cast<int>(idx));
}

double MaxTradableExternalQtyForOrder(const fob::venue::v1::VenueLiquidityCurve& curve,
                                      const fob::common::v1::Side side,
                                      const FlowOrder& order) {
    const auto* side_curve = SelectExternalCurve(curve, side);
    if (side_curve == nullptr || side_curve->q_grid().empty() || side_curve->p_of_q().empty()) {
        return 0.0;
    }

    const double limit_price = side == fob::common::v1::SIDE_BUY
        ? (double)order.p_high
        : -(double)order.p_high;

    double max_qty = 0.0;
    for (size_t i = 0; i < static_cast<size_t>(side_curve->q_grid_size()); ++i) {
        const size_t price_idx = std::min(i, static_cast<size_t>(side_curve->p_of_q_size() - 1));
        const double price = side_curve->p_of_q(static_cast<int>(price_idx));
        const bool acceptable = side == fob::common::v1::SIDE_BUY
            ? price <= limit_price + kQtyEps
            : price + kQtyEps >= limit_price;
        if (!acceptable) {
            break;
        }
        max_qty = side_curve->q_grid(static_cast<int>(i));
    }

    return max_qty;
}

double MaxPriceImprovingExternalQtyForOrder(
    const fob::venue::v1::VenueLiquidityCurve& curve,
    const fob::common::v1::Side side,
    const FlowOrder& order,
    const double internal_price) {
    const auto* side_curve = SelectExternalCurve(curve, side);
    if (side_curve == nullptr || side_curve->q_grid().empty() || side_curve->p_of_q().empty()) {
        return 0.0;
    }

    const double limit_price = side == fob::common::v1::SIDE_BUY
        ? (double)order.p_high
        : -(double)order.p_high;

    double max_qty = 0.0;
    for (size_t i = 0; i < static_cast<size_t>(side_curve->q_grid_size()); ++i) {
        const size_t price_idx = std::min(i, static_cast<size_t>(side_curve->p_of_q_size() - 1));
        const double price = side_curve->p_of_q(static_cast<int>(price_idx));
        const bool acceptable = side == fob::common::v1::SIDE_BUY
            ? price <= limit_price + kQtyEps
            : price + kQtyEps >= limit_price;
        if (!acceptable) {
            break;
        }

        const bool improves_internal = side == fob::common::v1::SIDE_BUY
            ? price + kQtyEps < internal_price
            : price > internal_price + kQtyEps;
        if (!improves_internal) {
            break;
        }

        max_qty = side_curve->q_grid(static_cast<int>(i));
    }

    return max_qty;
}

std::string ExternalConsumedKey(const std::string& symbol,
                                const fob::common::v1::Side side) {
    return symbol + "|" + std::to_string(static_cast<int>(side));
}

std::optional<double> ComputeInternalOverlapPrice(
    const std::vector<FlowOrder>& orders,
    const std::string& symbol
) {
    double max_sell_min = -std::numeric_limits<double>::infinity();
    double min_buy_max = std::numeric_limits<double>::infinity();
    bool has_buy = false;
    bool has_sell = false;

    for (const auto& order : orders) {
        const auto* leg = order.find_leg(symbol);
        if (leg == nullptr) continue;
        const double weight = (double)leg->weight;
        if (std::abs(weight) <= kQtyEps) continue;

        if (weight > 0.0) {
            has_buy = true;
            min_buy_max = std::min(min_buy_max, (double)order.p_high);
        } else {
            has_sell = true;
            const double sell_min = -(double)order.p_high;
            max_sell_min = std::max(max_sell_min, sell_min);
        }
    }

    if (!has_buy || !has_sell) return std::nullopt;
    if (max_sell_min > min_buy_max + kQtyEps) return std::nullopt;
    return 0.5 * (max_sell_min + min_buy_max);
}

void AddFill(fob::matching::v1::BatchResult* result,
             const FlowOrder& order,
             const std::string& symbol,
             const fob::common::v1::Side side,
             const double qty,
             const double price,
             const std::string& liquidity_source,
             const fob::venue::v1::VenueLiquidityCurve* curve = nullptr) {
    if (result == nullptr || qty <= kQtyEps) {
        return;
    }

    auto* fill = result->add_fills();
    fill->set_order_id(order.order_id);
    fill->set_user_id(order.user_id);
    {
        auto* inst = fill->mutable_instrument();
        inst->set_symbol(symbol);
        const auto sep = symbol.find('/');
        if (sep != std::string::npos) {
            inst->set_base(symbol.substr(0, sep));
            inst->set_quote(symbol.substr(sep + 1));
        }
    }
    fill->set_side(side);
    fill->set_liquidity_source(liquidity_source);
    fill->mutable_provenance()->set_liquidity_source(liquidity_source);
    if (curve != nullptr) {
        fill->mutable_provenance()->set_venue_id(curve->venue_id());
        fill->mutable_provenance()->set_snapshot_id(curve->snapshot_id());
        fill->mutable_provenance()->set_curve_id(CurveIdForVenueCurve(*curve));
    }
    *fill->mutable_executed_qty() = ToDecimal(qty).to_proto();
    *fill->mutable_price() = ToDecimal(price).to_proto();
    *fill->mutable_executed_notional() = ToDecimal(qty * price).to_proto();
}

}  // namespace

ContinuousClearingSolver::ContinuousClearingSolver() {
    cfg_.version = 0;
    cfg_.epsilon_liquidity = 1e-9;
    cfg_.batch_interval = std::chrono::milliseconds(1000);
    cfg_.tolerance = 1e-6;
    cfg_.max_iterations = 1000;
}

Eigen::VectorXd ContinuousClearingSolver::SolveImpl(
    Eigen::SparseMatrix<double> &W,
    Eigen::VectorXd &pi,
    Eigen::VectorXd &d,
    Eigen::VectorXd &qH,
    Eigen::VectorXd &pH
) {
    size_t size = qH.size();

    size_t assets = pi.size();
    size_t orders = size - 2 * assets;

    Eigen::VectorXd x = qH * 0.5;
    Eigen::VectorXd q_x = qH - x;
    
    Eigen::VectorXd mu = Eigen::VectorXd::Ones(size);
    Eigen::VectorXd lambda = Eigen::VectorXd::Ones(size);

    Eigen::VectorXd r_cp(assets);
    Eigen::VectorXd r_x(size);

    Eigen::VectorXd r_mu(size);
    Eigen::VectorXd r_lambda(size);
        
    Eigen::VectorXd r(size);
    Eigen::VectorXd eta(size);

    Eigen::VectorXd delta_cp(assets);
    Eigen::VectorXd delta_x(size);

    Eigen::VectorXd delta_mu(size);
    Eigen::VectorXd delta_lambda(size);

    Eigen::LLT<Eigen::MatrixXd> L;

    auto compute_alpha = [&]() -> double {
        double alpha_max = 1.0;
        for (size_t i = 0; i < size; ++i) {
            if (delta_x(i) < 0) {
                alpha_max = std::min(alpha_max, -x(i) / delta_x(i));
            } else if (delta_x(i) > 0) {
                alpha_max = std::min(alpha_max, q_x(i) / delta_x(i));
            }
            if (delta_mu(i) < 0) {
                alpha_max = std::min(alpha_max, -mu(i) / delta_mu(i));
            }
            if (delta_lambda(i) < 0) {
                alpha_max = std::min(alpha_max, -lambda(i) / delta_lambda(i));
            }
        }
        return 0.99 * alpha_max;
    };

    auto solver = [&]() {                
        delta_cp = L.solve(r_cp + W * (eta.cwiseProduct(r)));
        delta_x = eta.cwiseProduct(r - W.transpose() * delta_cp);

        delta_mu = (-r_mu - mu.cwiseProduct(delta_x)).cwiseQuotient(x);
        delta_lambda = (-r_lambda + lambda.cwiseProduct(delta_x)).cwiseQuotient(q_x);

        double alpha = compute_alpha();

        delta_cp *= alpha;
        delta_x *= alpha;

        delta_mu *= alpha;
        delta_lambda *= alpha;
    };

    auto predictor = [&]() -> bool {
        r_cp = W * x;
        r_x = pH - d.cwiseProduct(x) - W.transpose() * pi + mu - lambda;

        double solve_error = (W.leftCols(orders) * x.head(orders)).lpNorm<Eigen::Infinity>();
        double opt_error = (r_cp).lpNorm<Eigen::Infinity>()
                         + (r_x).lpNorm<Eigen::Infinity>()
                         + (lambda.dot(q_x) + mu.dot(x)) / (2.0 * size);
        opt_error /= 3;

        if ((opt_error < cfg_.tolerance) && (solve_error < cfg_.tolerance)) return false;

        r_mu = mu.cwiseProduct(x);
        r_lambda = lambda.cwiseProduct(q_x);

        r = r_x - mu + lambda;
        eta = (d + mu.cwiseQuotient(x) + lambda.cwiseQuotient(q_x)).cwiseInverse();
        
        Eigen::MatrixXd Lmatrix = W * eta.asDiagonal() * W.transpose();
        double diag_max = Lmatrix.diagonal().maxCoeff();
        double reg = 1e-12 * diag_max;
        Lmatrix.diagonal().array() += reg;
        L = Eigen::LLT<Eigen::MatrixXd>(std::move(Lmatrix));


        solver();
        return true;
    };

    auto corrector = [&]() {
        r_mu += delta_mu.cwiseProduct(delta_x);
        r_lambda += delta_lambda.cwiseProduct(delta_x);
            
        r = r_x - r_mu.cwiseQuotient(x) + r_lambda.cwiseQuotient(q_x);

        solver();
    };

    auto completer = [&]() {
        pi += delta_cp;

        x += delta_x;
        q_x = qH - x;

        mu += delta_mu;
        lambda += delta_lambda;
    };

    for (size_t iter = 0; iter < cfg_.max_iterations; ++iter) {
        if (!predictor()) break;
        corrector();
        completer();
    }

    return x.head(orders);
}

std::tuple<Eigen::SparseMatrix<double>, Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd>
ContinuousClearingSolver::Init(
    const std::vector<FlowOrder> &orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal> &reference_prices,
    const std::unordered_map<std::string, size_t> &prices_map
) {
    const size_t size = orders.size() + 2 * prices_map.size();

    Eigen::SparseMatrix<double> W(prices_map.size(), size);
    Eigen::VectorXd d(size);
    
    Eigen::VectorXd qH(size);
    Eigen::VectorXd pH(size);

    Eigen::VectorXd pi(prices_map.size());
    for (const auto &[sym, id] : prices_map) {
        const auto overlap = ComputeInternalOverlapPrice(orders, sym);
        if (overlap.has_value()) {
            pi(id) = *overlap;
            continue;
        }
        auto it = reference_prices.find(sym);
        pi(id) = (it == reference_prices.end())
            ? 0.0 : (double)cex::common::Decimal::from_proto(it->second);
    }

    for (size_t order = 0; order < orders.size(); ++order) {
        d(order) = (double)cex::common::Decimal::sub(orders[order].p_high, orders[order].p_low) / (double)orders[order].q_rate;

        pH(order) = (double)orders[order].p_high;
        qH(order) = std::min((double)orders[order].q_rate, (double)orders[order].remaining_qty());
    }

    Eigen::VectorXd exchange_qH = Eigen::VectorXd::Ones(pi.size());
    for (size_t order = 0; order < orders.size(); ++order) {
        double q_rate = (double)orders[order].q_rate;
        for (const auto &leg : orders[order].legs) {
            size_t asset = prices_map.at(leg.instrument_symbol);
            exchange_qH(asset) += std::abs((double)leg.weight) * q_rate;
        }
    }

    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        d(orders.size() + asset) = 1 / cfg_.epsilon_liquidity;

        pH(orders.size() + asset) = pi(asset);
        qH(orders.size() + asset) = exchange_qH(asset);
    }
    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        d(orders.size() + prices_map.size() + asset) = 1 / cfg_.epsilon_liquidity;

        pH(orders.size() + prices_map.size() + asset) = -pi(asset);
        qH(orders.size() + prices_map.size() + asset) = exchange_qH(asset);
    }

    std::vector<Eigen::Triplet<double>> triplets;

    for (size_t order = 0; order < orders.size(); ++order) {
        for (const auto &leg : orders[order].legs) {
            size_t asset = prices_map.at(leg.instrument_symbol);
            triplets.emplace_back(asset, order, (double)leg.weight);
        }
    }
    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        triplets.emplace_back(asset, orders.size() + asset, 1);
    }
    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        triplets.emplace_back(asset, orders.size() + prices_map.size() + asset, -1);
    }

    W.setFromTriplets(triplets.begin(), triplets.end());
    W.makeCompressed();

    return {std::move(W), std::move(pi), std::move(d), std::move(qH), std::move(pH)};
}

fob::matching::v1::BatchResult ContinuousClearingSolver::Solve(
    const std::vector<FlowOrder>& active_orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
    const ExternalLiquidityBySymbol& external_liquidity
) {
    std::unordered_map<std::string, size_t> prices_map;

    size_t id = 0;
    for (const auto &order : active_orders) {
        for (const auto &leg : order.legs) {
            if (!prices_map.contains(leg.instrument_symbol)) {
                prices_map[leg.instrument_symbol] = id++;
            }
        }
    }

    auto [W, pi, d, qH, pH] = Init(active_orders, reference_prices, prices_map);

    Eigen::VectorXd x = SolveImpl(W, pi, d, qH, pH);
    fob::matching::v1::BatchResult result;
    std::unordered_map<std::string, SymbolPlan> symbol_plans;
    std::unordered_map<std::string, double> symbol_total_qty;
    std::unordered_map<std::string, double> symbol_total_notional;
    std::unordered_map<std::string, double> symbol_external_consumed;

    size_t total_legs = 0;
    for (const auto& order : active_orders) {
        total_legs += order.legs.size();
    }
    std::vector<PlannedLegFill> planned_fills;
    planned_fills.reserve(total_legs);
    std::vector<double> actual_order_qty(active_orders.size(), 0.0);

    for (const auto& [sym, idx] : prices_map) {
        double price = pi(idx);
        (*result.mutable_clear_prices())[sym] = ToDecimal(price).to_proto();
    }

    for (size_t i = 0; i < active_orders.size(); ++i) {
        const auto& order = active_orders[i];
        double executed_rate = std::clamp(x(i), 0.0, qH(i));
        double executed_qty = executed_rate * cfg_.batch_interval.count() / 1000.0;
        double max_order_qty = qH(i) * cfg_.batch_interval.count() / 1000.0;

        for (const auto& leg : order.legs) {
            double leg_weight = std::abs((double)leg.weight);
            double leg_qty = executed_qty * leg_weight;
            if (leg_qty <= kQtyEps) {
                continue;
            }

            planned_fills.push_back(PlannedLegFill{
                .order = &order,
                .order_index = i,
                .symbol = leg.instrument_symbol,
                .side = leg.weight.units > 0 ? fob::common::v1::SIDE_BUY
                                             : fob::common::v1::SIDE_SELL,
                .order_qty = executed_qty,
                .max_leg_qty = max_order_qty * leg_weight,
                .weight_abs = leg_weight,
                .requested_leg_qty = leg_qty,
                .clear_price = pi(prices_map.at(leg.instrument_symbol)),
            });

            auto* planned = &planned_fills.back();
            auto& symbol_plan = symbol_plans[leg.instrument_symbol];
            symbol_plan.all_single_leg = symbol_plan.all_single_leg && order.legs.size() == 1;
            if (planned->side == fob::common::v1::SIDE_BUY) {
                symbol_plan.buys.push_back(planned);
            } else if (planned->side == fob::common::v1::SIDE_SELL) {
                symbol_plan.sells.push_back(planned);
            }
        }
    }

    double residual_norm = 0.0;
    int external_symbols_used = 0;

    for (auto& [symbol, plan] : symbol_plans) {
        double total_buy = 0.0;
        for (const auto* fill : plan.buys) total_buy += fill->requested_leg_qty;

        double total_sell = 0.0;
        for (const auto* fill : plan.sells) total_sell += fill->requested_leg_qty;

        if (!plan.all_single_leg) {
            for (auto* fill : plan.buys) {
                fill->actual_leg_qty = fill->requested_leg_qty;
                fill->internal_leg_qty = fill->requested_leg_qty;
                actual_order_qty[fill->order_index] = fill->order_qty;
            }
            for (auto* fill : plan.sells) {
                fill->actual_leg_qty = fill->requested_leg_qty;
                fill->internal_leg_qty = fill->requested_leg_qty;
                actual_order_qty[fill->order_index] = fill->order_qty;
            }
            residual_norm = std::max(residual_norm, std::abs(total_buy - total_sell));
            continue;
        }

        bool symbol_used_external = false;
        const auto curve_it = external_liquidity.find(symbol);

        const auto update_actual_order_qty = [&](const PlannedLegFill& fill) {
            if (fill.weight_abs <= kQtyEps) {
                return;
            }
            actual_order_qty[fill.order_index] = std::max(
                actual_order_qty[fill.order_index],
                fill.actual_leg_qty / fill.weight_abs
            );
        };

        const auto requested_remaining = [](const std::vector<PlannedLegFill*>& fills) {
            double total = 0.0;
            for (const auto* fill : fills) {
                total += std::max(0.0, fill->requested_leg_qty - fill->actual_leg_qty);
            }
            return total;
        };

        const auto capacity_remaining = [](const std::vector<PlannedLegFill*>& fills) {
            double total = 0.0;
            for (const auto* fill : fills) {
                total += std::max(0.0, fill->max_leg_qty - fill->actual_leg_qty);
            }
            return total;
        };

        const auto allocate_internal_side = [&](std::vector<PlannedLegFill*>& fills,
                                                const double target_total) {
            const double total_remaining = requested_remaining(fills);
            if (fills.empty() || total_remaining <= kQtyEps || target_total <= kQtyEps) {
                return;
            }

            const double ratio = std::clamp(target_total / total_remaining, 0.0, 1.0);
            for (auto* fill : fills) {
                const double remaining = std::max(
                    0.0,
                    fill->requested_leg_qty - fill->actual_leg_qty
                );
                const double qty = remaining * ratio;
                if (qty <= kQtyEps) {
                    continue;
                }
                fill->internal_leg_qty += qty;
                fill->actual_leg_qty += qty;
                update_actual_order_qty(*fill);
            }
        };

        const auto allocate_external_side = [&](std::vector<PlannedLegFill*>& fills,
                                                const fob::common::v1::Side side,
                                                const double target_total,
                                                const bool require_price_improvement) {
            if (curve_it == external_liquidity.end() || fills.empty() || target_total <= kQtyEps) {
                return 0.0;
            }

            double consumed = 0.0;
            for (const auto* fill : fills) {
                consumed += fill->external_leg_qty;
            }

            double allocated = 0.0;
            for (auto* fill : fills) {
                const double remaining_target = target_total - allocated;
                if (remaining_target <= kQtyEps) {
                    break;
                }

                const double remaining_capacity = std::max(
                    0.0,
                    fill->max_leg_qty - fill->actual_leg_qty
                );
                if (remaining_capacity <= kQtyEps) {
                    continue;
                }

                const double max_tradable = require_price_improvement
                    ? MaxPriceImprovingExternalQtyForOrder(
                          curve_it->second,
                          side,
                          *fill->order,
                          fill->clear_price)
                    : MaxTradableExternalQtyForOrder(curve_it->second, side, *fill->order);
                const double available = std::max(0.0, max_tradable - consumed);
                const double qty = std::min({remaining_target, remaining_capacity, available});
                if (qty <= kQtyEps) {
                    continue;
                }

                fill->external_leg_qty += qty;
                fill->actual_leg_qty += qty;
                update_actual_order_qty(*fill);
                consumed += qty;
                allocated += qty;
                symbol_used_external = true;
            }
            return allocated;
        };

        // Route to external liquidity first only when its curve improves the
        // internal clearing price. Otherwise local liquidity keeps priority.
        allocate_external_side(
            plan.buys,
            fob::common::v1::SIDE_BUY,
            capacity_remaining(plan.buys),
            true
        );
        allocate_external_side(
            plan.sells,
            fob::common::v1::SIDE_SELL,
            capacity_remaining(plan.sells),
            true
        );

        const double remaining_buy_for_internal = requested_remaining(plan.buys);
        const double remaining_sell_for_internal = requested_remaining(plan.sells);
        const double internal_total =
            std::min(remaining_buy_for_internal, remaining_sell_for_internal);

        allocate_internal_side(plan.buys, internal_total);
        allocate_internal_side(plan.sells, internal_total);

        const double remaining_buy_after_internal = requested_remaining(plan.buys);
        const double remaining_sell_after_internal = requested_remaining(plan.sells);

        if (remaining_buy_after_internal > remaining_sell_after_internal + kQtyEps) {
            allocate_external_side(
                plan.buys,
                fob::common::v1::SIDE_BUY,
                capacity_remaining(plan.buys),
                false
            );
        } else if (remaining_sell_after_internal > remaining_buy_after_internal + kQtyEps) {
            allocate_external_side(
                plan.sells,
                fob::common::v1::SIDE_SELL,
                capacity_remaining(plan.sells),
                false
            );
        }

        residual_norm = std::max(
            residual_norm,
            std::abs(requested_remaining(plan.buys) - requested_remaining(plan.sells))
        );

        if (symbol_used_external) {
            ++external_symbols_used;
        }
    }

    for (const auto& planned : planned_fills) {
        if (planned.internal_leg_qty > kQtyEps) {
            AddFill(
                &result,
                *planned.order,
                planned.symbol,
                planned.side,
                planned.internal_leg_qty,
                planned.clear_price,
                "internal"
            );
            symbol_total_qty[planned.symbol] += planned.internal_leg_qty;
            symbol_total_notional[planned.symbol] += planned.internal_leg_qty * planned.clear_price;
        }

        if (planned.external_leg_qty <= kQtyEps) {
            continue;
        }

        const auto curve_it = external_liquidity.find(planned.symbol);
        if (curve_it == external_liquidity.end()) {
            continue;
        }

        const std::string consumed_key = ExternalConsumedKey(planned.symbol, planned.side);
        symbol_external_consumed[consumed_key] += planned.external_leg_qty;
        double external_price = planned.clear_price;
        const auto maybe_price = ExternalPriceAtQty(
            curve_it->second,
            planned.side,
            symbol_external_consumed[consumed_key]
        );
        if (maybe_price.has_value()) {
            external_price = *maybe_price;
        }

        AddFill(
            &result,
            *planned.order,
            planned.symbol,
            planned.side,
            planned.external_leg_qty,
            external_price,
            LiquiditySourceForVenue(curve_it->second.venue_id()),
            &curve_it->second
        );
        symbol_total_qty[planned.symbol] += planned.external_leg_qty;
        symbol_total_notional[planned.symbol] += planned.external_leg_qty * external_price;
    }

    for (size_t i = 0; i < active_orders.size(); ++i) {
        const auto& order = active_orders[i];
        double executed_qty = actual_order_qty[i];
        if (executed_qty <= kQtyEps) {
            continue;
        }

        double executed_rate = executed_qty * 1000.0 / cfg_.batch_interval.count();
        (*result.mutable_executed_rates())[order.order_id] = ToDecimal(executed_rate).to_proto();

        double remaining_qty_double = (double)order.remaining_qty();
        double new_remaining_double = std::max(0.0, remaining_qty_double - executed_qty);

        auto* update = result.add_order_updates();
        update->set_order_id(order.order_id);
        if (new_remaining_double <= kQtyEps) {
            update->set_status(fob::common::v1::ORDER_STATUS_FILLED);
        } else {
            update->set_status(fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
        }

        *update->mutable_remaining_qty() = ToDecimal(new_remaining_double).to_proto();
        *update->mutable_filled_qty_total() = ToDecimal((double)order.filled_cum + executed_qty).to_proto();
        *update->mutable_updated_at() = cex::common::now_ts();
    }

    for (const auto& [sym, total_qty] : symbol_total_qty) {
        if (total_qty <= kQtyEps) {
            continue;
        }
        (*result.mutable_clear_prices())[sym] =
            ToDecimal(symbol_total_notional[sym] / total_qty).to_proto();
    }

    auto* diag = result.mutable_diagnostics();
    diag->set_residual_norm(residual_norm);
    diag->set_num_active_orders(static_cast<uint32_t>(active_orders.size()));
    diag->set_config_version(cfg_.version);
    diag->set_solver_diagnostics_json(
        "{\"kind\":\"continuous_clearing\",\"external_symbols_used\":" +
        std::to_string(external_symbols_used) + "}"
    );

    return result;
}

void ContinuousClearingSolver::SetSolverConfig(SolverConfig cfg) {
    cfg_ = std::move(cfg);
}

}  // namespace cex::matching::domain
