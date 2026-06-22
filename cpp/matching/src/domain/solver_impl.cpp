// ============================================================================
// solver_impl.cpp — ядро F-04: ContinuousClearingSolver.
//
// Физический смысл:
//   Это математическое сердце Flow Order Book. Берёт snapshot активных
//   FlowOrder и решает задачу непрерывного клиринга:
//     "найти равновесные цены π[symbol] и executed rates x[order]
//      так, чтобы максимизировать совокупный объём исполнения
//      при соблюдении бюджетных и speed-constraint'ов каждой заявки."
//
//   Математически — это квадратичная задача (QP) с inequality constraints,
//   которую мы решаем interior-point методом (predictor-corrector) с
//   разложением Холесского (LLT) для нормальной системы.
//
// Canonical mathematical foundation:
//   - IN-012 (academic note 2026-04-15, 44 pages) — Lagrangian/Hamiltonian
//     description, 4 equivalent curve forms, clearing as Lagrange
//     multiplier for flow balance constraint.
//   - ADR-035 (docs/03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md)
//     — fixes Hamiltonian-based formulation as canonical choice;
//     Mehrotra interior-point — numerical implementation, not alternative.
//   - solver-foundation.md (docs/09-implementation/) — full mapping
//     IN-012 objects ↔ C++ variables в этом файле + test vectors.
//   - business-rules.md §Clearing Mechanics (R-CLR-001..008) — invariants.
//   - entities.md §Continuous-Order Primitives — agent typology
//     (StandardAgent, PortfolioAgent, LinearFeeAgent, etc).
//
// Mapping в этот код:
//   - π (Eigen::VectorXd) ≡ P* clearing price (Lagrange multiplier for
//     W·x = 0 constraint, IN-012 §4.3).
//   - W matrix ≡ asset/order incidence (Σ_i Q̇_i = 0 across asset rows).
//   - d diagonal ≡ Σ_i M_i^(-1) inertias inverse, после rescaling.
//   - pH vector ≡ external anchors a_i (IN-012 §4.5).
//   - x[i] ≡ V_i(Q_i, P*) — желаемая скорость агента i.
//   - Convergence test: ‖W·x‖_∞ < tol (clearing constraint, IN-012 4.2).
//
// Структура файла:
//   1. anonymous namespace — helper-структуры PlannedLegFill / SymbolPlan,
//      ExternalPriceAtQty / MaxTradableExternalQtyForOrder и т.д.
//   2. SolveImpl(W, π, d, qH, pH) — чистый numerical QP solver.
//   3. Init() — построение QP (sparse W, vectors d, qH, pH, π).
//   4. Solve() — главный entry point: Init → SolveImpl → fallback (PR-F02-007)
//      → 3-фазное распределение (external improve → internal cross →
//      external residual) → BatchResult.
//
// QP формулировка (упрощённо):
//   Variables: x ∈ R^N (rates по orders + virtual buy/sell ассетов).
//   Objective: maximize sum_i (pH_i * x_i) - 0.5 * d_i * x_i^2.
//   Constraints:
//     - 0 ≤ x_i ≤ qH_i              (box bounds, μ, λ — multipliers)
//     - W * x == 0                  (rate balance per asset, π — equality multiplier)
//
//   KKT-условия:
//     pH - d*x - W^T*π + μ - λ = 0     (stationarity)
//     W*x = 0                            (primal feasibility, asset balance)
//     0 ≤ x ≤ qH, μ, λ ≥ 0              (dual + bounds)
//     μ_i * x_i = 0, λ_i * (qH-x)_i = 0  (complementarity)
//
//   Interior-point: ослабляем complementarity (μ_i * x_i = σ * τ) и идём
//   к стационарной точке Newton-step'ами.
//
// PR-F02-007 fallback:
//   LLT-based solver численно хрупок на degenerate cases (identical orders,
//   pi кучкуются на zero и т.п.). При NaN мы откатываемся на:
//     x  := qH        (каждая заявка просит свой максимум)
//     pi := initial   (overlap mid или reference price)
//   Это позволяет downstream allocation выдать осмысленные fills вместо
//   freeze всего book'а.
// ============================================================================

#include "domain/solver_impl.hpp"

#include <algorithm>          // std::min/max/clamp/sort
#include <cmath>              // std::isfinite, std::abs, std::llround
#include <limits>             // numeric_limits<double>::infinity()
#include <optional>           // std::optional для возврата "no price"
#include <string>
#include <unordered_map>
#include <utility>            // std::move
#include <vector>

#include "cex/common/decimal.hpp"   // Decimal — money
#include "cex/common/log.hpp"
#include "cex/common/time.hpp"

namespace cex::matching::domain {

namespace {

/// Числовой epsilon для qty-сравнений (1e-12 base units). Любое qty ниже —
/// считается нулём. Не использовать для денежных сравнений (там Decimal::cmp).
constexpr double kQtyEps = 1e-12;

/// Структура одного запланированного fill в рамках одного symbol.
/// Заполняется на этапе planning, затем модифицируется allocation phase'ами.
/// order/order_index — связь с FlowOrder в snapshot'е.
struct PlannedLegFill {
    const FlowOrder* order{nullptr};       ///< Указатель на исходную заявку (не owning)
    size_t order_index{0};                 ///< Индекс заявки в active_orders vector
    std::string symbol;
    fob::common::v1::Side side{fob::common::v1::SIDE_UNSPECIFIED};
    double order_qty{0.0};                 ///< Сколько заявка хочет в этом батче (executed_qty)
    double max_leg_qty{0.0};               ///< Максимум для этой ноги (qH * leg_weight)
    double weight_abs{0.0};                ///< |leg.weight| — для multi-leg orders
    double requested_leg_qty{0.0};         ///< executed_qty * weight_abs — что должна получить нога
    double clear_price{0.0};               ///< π[symbol] для этого fill
    double actual_leg_qty{0.0};            ///< Реально аллоцированное (internal + external)
    double internal_leg_qty{0.0};          ///< Часть из internal cross
    double external_leg_qty{0.0};          ///< Часть из external venue
};

/// План по одному symbol: buys и sells fills, все ли single-leg.
/// multi-leg orders (F-09) обрабатываются отдельной branch'ей.
struct SymbolPlan {
    std::vector<PlannedLegFill*> buys;
    std::vector<PlannedLegFill*> sells;
    bool all_single_leg{true};
};

/// double → Decimal{units, scale=12} с round-to-nearest. 1e-12 точность —
/// достаточно для base/quote units на типичных парах. llround — round
/// to nearest int64, не truncation.
static cex::common::Decimal ToDecimal(double dbl) {
    return {static_cast<int64_t>(std::llround(dbl * 1e12)), 12};
}

/// Классификация источника ликвидности для liquidity_source поля fill'а.
/// uniswap/dex в venue_id → "dex_hedge", иначе "cex_hedge".
/// Используется для post-trade analytics и F-12 routing.
std::string LiquiditySourceForVenue(const std::string& venue_id) {
    if (venue_id.find("uniswap") != std::string::npos ||
        venue_id.find("dex") != std::string::npos) {
        return "dex_hedge";
    }
    return "cex_hedge";
}

/// curve_id для audit (тот же паттерн что и в run_batch_uc.cpp).
std::string CurveIdForVenueCurve(const fob::venue::v1::VenueLiquidityCurve& curve) {
    if (!curve.curve_id().empty()) {
        return curve.curve_id();
    }
    if (curve.has_meta() && !curve.meta().event_id().empty()) {
        return curve.meta().event_id();
    }
    return "";
}

/// Выбор bid/ask side кривой по нашей стороне:
///   BUY (мы покупаем) → ask_curve (предложение venue)
///   SELL (мы продаём) → bid_curve (спрос venue)
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

/// Цена на venue curve для заданного qty. Линейный поиск первого q_grid[i]
/// ≥ qty (степь — curve sorted by qty), возвращает p_of_q[i]. Если qty
/// больше всего grid'а — берём последнюю точку (max liquidity).
/// std::nullopt — кривая пуста (venue down / no liquidity).
std::optional<double> ExternalPriceAtQty(const fob::venue::v1::VenueLiquidityCurve& curve,
                                         const fob::common::v1::Side side,
                                         const double qty) {
    const auto* side_curve = SelectExternalCurve(curve, side);
    if (side_curve == nullptr || side_curve->q_grid().empty() || side_curve->p_of_q().empty()) {
        return std::nullopt;
    }

    // Default — последняя точка (если qty > всего grid'а).
    size_t idx = side_curve->q_grid_size() - 1;
    for (size_t i = 0; i < static_cast<size_t>(side_curve->q_grid_size()); ++i) {
        if (side_curve->q_grid(static_cast<int>(i)) + kQtyEps >= qty) {
            idx = i;
            break;
        }
    }

    // Защита от mismatched-sizes (если p_of_q короче чем q_grid).
    if (idx >= static_cast<size_t>(side_curve->p_of_q_size())) {
        idx = static_cast<size_t>(side_curve->p_of_q_size() - 1);
    }

    return side_curve->p_of_q(static_cast<int>(idx));
}

/// Максимальный qty, который заявка может executed на venue по
/// limit_price (= order.p_high для BUY, -order.p_high для SELL в signed
/// domain). Идёт по q_grid пока price acceptable, возвращает крайнюю
/// точку. Используется в фазе external-residual allocation.
double MaxTradableExternalQtyForOrder(const fob::venue::v1::VenueLiquidityCurve& curve,
                                      const fob::common::v1::Side side,
                                      const FlowOrder& order) {
    const auto* side_curve = SelectExternalCurve(curve, side);
    if (side_curve == nullptr || side_curve->q_grid().empty() || side_curve->p_of_q().empty()) {
        return 0.0;
    }

    // limit_price учитывает signed-by-side convention в domain (см. PR-F02-005).
    const double limit_price = side == fob::common::v1::SIDE_BUY
        ? (double)order.p_high
        : -(double)order.p_high;

    double max_qty = 0.0;
    for (size_t i = 0; i < static_cast<size_t>(side_curve->q_grid_size()); ++i) {
        const size_t price_idx = std::min(i, static_cast<size_t>(side_curve->p_of_q_size() - 1));
        const double price = side_curve->p_of_q(static_cast<int>(price_idx));
        // BUY acceptable: price ≤ limit (мы готовы платить до limit).
        // SELL acceptable: price ≥ limit (мы хотим получить не менее).
        const bool acceptable = side == fob::common::v1::SIDE_BUY
            ? price <= limit_price + kQtyEps
            : price + kQtyEps >= limit_price;
        if (!acceptable) {
            break;   // кривая monotone — дальше не acceptable
        }
        max_qty = side_curve->q_grid(static_cast<int>(i));
    }

    return max_qty;
}

/// То же что MaxTradableExternalQtyForOrder, но дополнительно требует
/// price improvement vs internal_price. Используется в фазе external-improve
/// (приоритет venue над internal только если venue ДЕШЕВЛЕ).
///
/// BUY: external price < internal_price (купить дешевле).
/// SELL: external price > internal_price (продать дороже).
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

        // Дополнительный гейт: improves_internal.
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

/// Speed band scaling для single-leg заявки: при заданной цене сколько
/// quantity мы готовы исполнить за batch_interval.
///
/// Физический смысл:
///   FlowOrder задаёт q_rate (max units/sec) и [p_low, p_high]. На цене
///   p_high мы хотим исполняться на max rate, на p_low — нулевая скорость
///   (мы не хотим хуже). Линейная интерполяция между.
///
/// BUY: ratio = (p_high - price) / (p_high - p_low). Чем ниже price (лучше
///      для нас), тем больше мы готовы купить.
/// SELL: ratio = (price - p_low) / (p_high - p_low). Чем выше price, тем
///       больше мы готовы продать.
///
/// Multi-leg orders → infinity (используется multi-leg код, не speed-band).
double PriceBandMaxQtyForSingleLegOrder(const FlowOrder& order,
                                        const fob::common::v1::Side side,
                                        const double price,
                                        const std::chrono::milliseconds batch_interval) {
    if (order.legs.size() != 1 || !std::isfinite(price)) {
        return std::numeric_limits<double>::infinity();
    }

    double low = (double)order.p_low;
    double high = (double)order.p_high;
    if (!std::isfinite(low) || !std::isfinite(high)) {
        return std::numeric_limits<double>::infinity();
    }

    // Для SELL заявок в domain p_low/p_high signed-negated (PR-F02-005).
    // Возвращаем к "business" значениям перед интерполяцией.
    if (low < 0.0 && high < 0.0) {
        low = -(double)order.p_high;
        high = -(double)order.p_low;
    }
    if (low > high) {
        std::swap(low, high);
    }

    const double width = high - low;
    if (width <= kQtyEps) {
        return std::numeric_limits<double>::infinity();   // degenerate p_low == p_high
    }

    double ratio = 1.0;
    if (side == fob::common::v1::SIDE_BUY) {
        ratio = (high - price) / width;
    } else if (side == fob::common::v1::SIDE_SELL) {
        ratio = (price - low) / width;
    }
    ratio = std::clamp(ratio, 0.0, 1.0);

    const double max_rate = (double)order.q_rate;
    // Перевод rate (units/sec) в quantity per batch: rate * interval_sec.
    return max_rate * ratio * batch_interval.count() / 1000.0;
}

/// Ключ для аккумулятора consumed external liquidity per (symbol, side).
/// BUY/SELL потоки не overlap, нужны отдельные счётчики.
std::string ExternalConsumedKey(const std::string& symbol,
                                const fob::common::v1::Side side) {
    return symbol + "|" + std::to_string(static_cast<int>(side));
}

/// Internal overlap mid-price: если BUY-заявки и SELL-заявки пересекаются
/// по диапазону цен, берём середину overlap region. Используется как
/// initial π[symbol] вместо reference_price при наличии overlap.
///
/// Физический смысл:
///   max_sell_min — наибольший lower bound у SELL'ов (минимум за который
///                  готовы продавать).
///   min_buy_max  — наименьший upper bound у BUY'ов (максимум который
///                  готовы платить).
///   Overlap: max_sell_min ≤ min_buy_max. Mid = 0.5 * (sum).
///
/// std::nullopt — нет overlap (только BUY'ы или только SELL'ы) или
/// диапазоны не пересекаются.
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
            // BUY: ограничение сверху — p_high.
            has_buy = true;
            min_buy_max = std::min(min_buy_max, (double)order.p_high);
        } else {
            // SELL: в domain p_high уже negated, поэтому sell_min = -p_high.
            has_sell = true;
            const double sell_min = -(double)order.p_high;
            max_sell_min = std::max(max_sell_min, sell_min);
        }
    }

    if (!has_buy || !has_sell) return std::nullopt;
    if (max_sell_min > min_buy_max + kQtyEps) return std::nullopt;
    return 0.5 * (max_sell_min + min_buy_max);
}

/// Добавляет fill в BatchResult с правильным заполнением всех полей
/// (instrument с разбором symbol на base/quote, liquidity_source,
/// provenance, executed_qty/price/notional).
/// Curve — optional для external fills.
void AddFill(fob::matching::v1::BatchResult* result,
             const FlowOrder& order,
             const std::string& symbol,
             const fob::common::v1::Side side,
             const double qty,
             const double price,
             const std::string& liquidity_source,
             const fob::venue::v1::VenueLiquidityCurve* curve = nullptr) {
    if (result == nullptr || qty <= kQtyEps) {
        return;   // защита от 0-qty fills (mess up downstream invariants)
    }

    auto* fill = result->add_fills();
    fill->set_order_id(order.order_id);
    fill->set_user_id(order.user_id);
    {
        // Разбор "BASE/QUOTE" на отдельные поля для downstream.
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
    // notional = qty * price — pre-computed чтобы downstream не считал.
    *fill->mutable_executed_notional() = ToDecimal(qty * price).to_proto();
}

}  // namespace

/// Конструктор с дефолтными параметрами. epsilon_liquidity маленький — это
/// "стоимость" virtual ассет-buy/sell слотов (high penalty для нарушения
/// asset balance constraint). batch_interval = 1 сек — типичный dev tick.
ContinuousClearingSolver::ContinuousClearingSolver() {
    cfg_.version = 0;
    cfg_.epsilon_liquidity = 1e-9;
    cfg_.batch_interval = std::chrono::milliseconds(1000);
    cfg_.tolerance = 1e-6;
    cfg_.max_iterations = 1000;
}

// ============================================================================
// SolveImpl — численное ядро interior-point QP solver'а.
//
// Входы (всё mutable references — модифицируются):
//   W  — sparse incidence matrix: W[asset, order] = ±weight ноги.
//        Дополнительно W[asset, virtual_buy] = +1, W[asset, virtual_sell] = -1.
//   π  — vector цен per asset (size = N_assets).
//   d  — diagonal "стоимости" (penalty / curvature): для orders = (p_high-p_low)/q_rate,
//        для virtual ассетов = 1/epsilon_liquidity.
//   qH — vector upper bounds: для orders = min(q_rate, remaining/interval),
//        для virtual = aggregate exchange_qH.
//   pH — vector "ожидаемых выигрышей" per variable.
//
// Выход: x — vector rates для orders (head(orders) часть полного size).
//
// Algorithm: predictor-corrector (Mehrotra-style) interior-point.
//   * predictor: вычисляет direction Newton-step'ом, измеряет error.
//   * corrector: уточняет с учётом complementarity products.
//   * completer: применяет step.
// ============================================================================
Eigen::VectorXd ContinuousClearingSolver::SolveImpl(
    Eigen::SparseMatrix<double> &W,
    Eigen::VectorXd &pi,
    Eigen::VectorXd &d,
    Eigen::VectorXd &qH,
    Eigen::VectorXd &pH
) {
    size_t size = qH.size();

    // size = N_orders + 2*N_assets (orders + virtual_buy + virtual_sell).
    size_t assets = pi.size();
    size_t orders = size - 2 * assets;

    // Initial point — interior: x = qH/2, μ = λ = 1 для всех.
    // q_x = qH - x — оставшийся "headroom" до upper bound.
    Eigen::VectorXd x = qH * 0.5;
    Eigen::VectorXd q_x = qH - x;

    Eigen::VectorXd mu = Eigen::VectorXd::Ones(size);
    Eigen::VectorXd lambda = Eigen::VectorXd::Ones(size);

    // Residuals для KKT.
    Eigen::VectorXd r_cp(assets);     // primal feasibility (W*x = 0)
    Eigen::VectorXd r_x(size);         // stationarity (gradient = 0)

    Eigen::VectorXd r_mu(size);        // complementarity μ.*x
    Eigen::VectorXd r_lambda(size);    // complementarity λ.*(qH-x)

    Eigen::VectorXd r(size);           // combined RHS для KKT системы
    Eigen::VectorXd eta(size);         // diagonal scaling factor

    // Step directions.
    Eigen::VectorXd delta_cp(assets);
    Eigen::VectorXd delta_x(size);

    Eigen::VectorXd delta_mu(size);
    Eigen::VectorXd delta_lambda(size);

    // LLT (Cholesky) разложение для нормальной системы W * eta * W^T.
    // Symmetric positive-definite — LLT идеально (быстрее LDLT/QR).
    Eigen::LLT<Eigen::MatrixXd> L;

    // -----------------------------------------------------------------------
    // compute_alpha — line-search: максимальный step factor 0 < α ≤ 0.99,
    // такой что после step остаёмся в feasible region (x ∈ [0, qH], μ, λ ≥ 0).
    //
    // Для каждого i: проверяем границу по каждой переменной, берём минимум.
    // 0.99 factor — central-path adherence (не подходим вплотную к границе).
    // -----------------------------------------------------------------------
    auto compute_alpha = [&]() -> double {
        double alpha_max = 1.0;
        for (size_t i = 0; i < size; ++i) {
            // x не должен стать < 0
            if (delta_x(i) < 0) {
                alpha_max = std::min(alpha_max, -x(i) / delta_x(i));
            } else if (delta_x(i) > 0) {
                // x не должен превысить qH
                alpha_max = std::min(alpha_max, q_x(i) / delta_x(i));
            }
            // μ не должен стать < 0
            if (delta_mu(i) < 0) {
                alpha_max = std::min(alpha_max, -mu(i) / delta_mu(i));
            }
            // λ не должен стать < 0
            if (delta_lambda(i) < 0) {
                alpha_max = std::min(alpha_max, -lambda(i) / delta_lambda(i));
            }
        }
        return 0.99 * alpha_max;   // отступаем от границы
    };

    // -----------------------------------------------------------------------
    // solver — решает редуцированную KKT-систему за один step.
    //
    // Идея: матрица KKT block-arrow, элиминируем delta_x → нормальная
    // система W * eta * W^T * delta_cp = r_cp + ... Решаем через L.solve.
    // Затем восстанавливаем delta_x, delta_mu, delta_lambda.
    //
    // Step scaling через compute_alpha.
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // predictor — вычисляет residuals и factorizes Hessian.
    //
    // r_cp = W * x       — primal residual (нарушение asset balance)
    // r_x  = pH - d.*x - W^T*π + μ - λ — gradient (stationarity error)
    //
    // Если оба residuals + complementarity ниже tolerance — convergence,
    // возвращаем false (главный цикл break'ает).
    //
    // Иначе строим η = (d + μ/x + λ/(qH-x))^-1 — scale factors,
    // формируем нормальную матрицу Lmatrix = W * η * W^T,
    // регуляризируем диагональ (1e-12 * max_diag) для numerical stability,
    // factorize LLT, и делаем step через solver().
    // -----------------------------------------------------------------------
    auto predictor = [&]() -> bool {
        r_cp = W * x;
        r_x = pH - d.cwiseProduct(x) - W.transpose() * pi + mu - lambda;

        // Solve error (asset imbalance) + opt_error (gradient + duality gap).
        // lpNorm<Infinity> — sup-norm. Tolerance — bridge для termination.
        double solve_error = (W.leftCols(orders) * x.head(orders)).lpNorm<Eigen::Infinity>();
        double opt_error = (r_cp).lpNorm<Eigen::Infinity>()
                         + (r_x).lpNorm<Eigen::Infinity>()
                         + (lambda.dot(q_x) + mu.dot(x)) / (2.0 * size);
        opt_error /= 3;

        if ((opt_error < cfg_.tolerance) && (solve_error < cfg_.tolerance)) return false;

        // Complementarity products — для corrector phase.
        r_mu = mu.cwiseProduct(x);
        r_lambda = lambda.cwiseProduct(q_x);

        // Combined RHS и diagonal scaling.
        r = r_x - mu + lambda;
        eta = (d + mu.cwiseQuotient(x) + lambda.cwiseQuotient(q_x)).cwiseInverse();

        // Normal equation матрица (W * diag(η) * W^T). Регуляризация
        // диагонали — стандартный приём для LLT robustness.
        Eigen::MatrixXd Lmatrix = W * eta.asDiagonal() * W.transpose();
        double diag_max = Lmatrix.diagonal().maxCoeff();
        double reg = 1e-12 * diag_max;
        Lmatrix.diagonal().array() += reg;
        L = Eigen::LLT<Eigen::MatrixXd>(std::move(Lmatrix));


        solver();
        return true;
    };

    // -----------------------------------------------------------------------
    // corrector — добавляет нелинейный complementarity-correction
    // (Mehrotra). Без него interior-point медленнее сходится.
    // -----------------------------------------------------------------------
    auto corrector = [&]() {
        r_mu += delta_mu.cwiseProduct(delta_x);
        r_lambda += delta_lambda.cwiseProduct(delta_x);

        r = r_x - r_mu.cwiseQuotient(x) + r_lambda.cwiseQuotient(q_x);

        solver();
    };

    // -----------------------------------------------------------------------
    // completer — применяет step ко всем переменным.
    // π += δπ, x += δx, μ += δμ, λ += δλ.
    // -----------------------------------------------------------------------
    auto completer = [&]() {
        pi += delta_cp;

        x += delta_x;
        q_x = qH - x;

        mu += delta_mu;
        lambda += delta_lambda;
    };

    // Главный interior-point цикл. До max_iterations или convergence.
    for (size_t iter = 0; iter < cfg_.max_iterations; ++iter) {
        if (!predictor()) break;
        corrector();
        completer();
    }

    // Возвращаем только rates по orders (первые N_orders элементов).
    // Virtual buy/sell нам не интересны — это только для balance constraint.
    return x.head(orders);
}

// ============================================================================
// Init — строит QP-задачу из FlowOrder snapshot'а и reference prices.
//
// Возвращает tuple {W, π, d, qH, pH} — переменные SolveImpl().
//
// Этапы:
//   1. Initial π: для каждого symbol — overlap mid или reference price.
//   2. Per-order: d_i = (p_high - p_low)/q_rate, pH_i = p_high,
//      qH_i = min(q_rate, remaining/interval).
//   3. Per-asset virtual buy/sell: d = 1/eps_liq, pH = ±π, qH = aggregate.
//   4. Sparse W triplets: per leg weight, per virtual ±1.
// ============================================================================
std::tuple<Eigen::SparseMatrix<double>, Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd>
ContinuousClearingSolver::Init(
    const std::vector<FlowOrder> &orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal> &reference_prices,
    const std::unordered_map<std::string, size_t> &prices_map
) {
    // Total size = N_orders + 2*N_assets (virtual buy + virtual sell per asset).
    const size_t size = orders.size() + 2 * prices_map.size();

    Eigen::SparseMatrix<double> W(prices_map.size(), size);
    Eigen::VectorXd d(size);

    Eigen::VectorXd qH(size);
    Eigen::VectorXd pH(size);

    Eigen::VectorXd pi(prices_map.size());
    // Initial π: prefer internal overlap mid, fallback на reference, fallback 0.
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

    // Per-order: d, pH, qH.
    for (size_t order = 0; order < orders.size(); ++order) {
        // d = price band width / max rate — "curvature" в QP.
        // Больше band ИЛИ меньше rate → большая d → больший penalty за rate.
        d(order) = (double)cex::common::Decimal::sub(orders[order].p_high, orders[order].p_low) / (double)orders[order].q_rate;

        pH(order) = (double)orders[order].p_high;
        // qH = min(q_rate, remaining/interval_sec). Limits rate per batch.
        const double interval_seconds = std::max(1e-9, cfg_.batch_interval.count() / 1000.0);
        qH(order) = std::min(
            (double)orders[order].q_rate,
            (double)orders[order].remaining_qty() / interval_seconds
        );
    }

    // Aggregate exchange_qH per asset = 1 + sum(|weight| * q_rate).
    // Это "максимальный flow" virtual buy/sell может покрыть.
    Eigen::VectorXd exchange_qH = Eigen::VectorXd::Ones(pi.size());
    for (size_t order = 0; order < orders.size(); ++order) {
        double q_rate = (double)orders[order].q_rate;
        for (const auto &leg : orders[order].legs) {
            size_t asset = prices_map.at(leg.instrument_symbol);
            exchange_qH(asset) += std::abs((double)leg.weight) * q_rate;
        }
    }

    // Virtual buy assets: pH = π (хотим купить по текущей цене).
    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        d(orders.size() + asset) = 1 / cfg_.epsilon_liquidity;   // high penalty

        pH(orders.size() + asset) = pi(asset);
        qH(orders.size() + asset) = exchange_qH(asset);
    }
    // Virtual sell assets: pH = -π (хотим продать).
    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        d(orders.size() + prices_map.size() + asset) = 1 / cfg_.epsilon_liquidity;

        pH(orders.size() + prices_map.size() + asset) = -pi(asset);
        qH(orders.size() + prices_map.size() + asset) = exchange_qH(asset);
    }

    // Sparse W triplets: order legs + virtual buy/sell columns.
    std::vector<Eigen::Triplet<double>> triplets;

    // Per leg: W[asset, order] = weight (signed).
    for (size_t order = 0; order < orders.size(); ++order) {
        for (const auto &leg : orders[order].legs) {
            size_t asset = prices_map.at(leg.instrument_symbol);
            triplets.emplace_back(asset, order, (double)leg.weight);
        }
    }
    // Virtual buy: W[asset, virtual_buy] = +1.
    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        triplets.emplace_back(asset, orders.size() + asset, 1);
    }
    // Virtual sell: W[asset, virtual_sell] = -1.
    for (size_t asset = 0; asset < prices_map.size(); ++asset) {
        triplets.emplace_back(asset, orders.size() + prices_map.size() + asset, -1);
    }

    W.setFromTriplets(triplets.begin(), triplets.end());
    W.makeCompressed();   // compact CSR-form для быстрого matmul

    return {std::move(W), std::move(pi), std::move(d), std::move(qH), std::move(pH)};
}

// ============================================================================
// Solve — главный entry point. Берёт snapshot заявок, reference prices и
// external liquidity, возвращает полный BatchResult.
//
// Шаги:
//   1. Build prices_map (symbol → asset index).
//   2. Init() → {W, π, d, qH, pH}.
//   3. SolveImpl() → x (rates).
//   4. PR-F02-007 fallback: если x или π имеют NaN → x=qH, π=initial.
//   5. PR-F02-004 diag log.
//   6. Build PlannedLegFill для каждой ноги.
//   7. Per-symbol: 3-фазное распределение
//        (external improve → internal cross → external residual).
//   8. AddFill для internal и external частей.
//   9. order_updates + executed_rates + diagnostics.
// ============================================================================
fob::matching::v1::BatchResult ContinuousClearingSolver::Solve(
    const std::vector<FlowOrder>& active_orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
    const ExternalLiquidityBySymbol& external_liquidity
) {
    std::unordered_map<std::string, size_t> prices_map;

    // Собираем уникальные symbol → index, в порядке появления.
    size_t id = 0;
    for (const auto &order : active_orders) {
        for (const auto &leg : order.legs) {
            if (!prices_map.contains(leg.instrument_symbol)) {
                prices_map[leg.instrument_symbol] = id++;
            }
        }
    }

    auto [W, pi, d, qH, pH] = Init(active_orders, reference_prices, prices_map);
    // Save initial pi from Init (overlap-based or reference-price fallback)
    // so we can restore it if SolveImpl diverges to NaN below.
    const Eigen::VectorXd initial_pi = pi;

    Eigen::VectorXd x = SolveImpl(W, pi, d, qH, pH);

    // PR-F02-007: numerical fallback when the QP solver diverges.
    // SolveImpl uses an Eigen LLT-based interior-point loop that is
    // numerically fragile around small remaining_qty / clustered identical
    // orders / certain symmetric BUY+SELL combos — empirically it returns
    // NaN for 3+ orders with identical (p_low, p_high, q_rate) on a single
    // symbol, and for asymmetric remaining (BUY 0.0003 + SELL 0.001) after
    // prior partial fills. In every case fills collapse to zero across the
    // whole batch because downstream `clamp(NaN, 0, qH) = 0` and `pi=NaN`
    // poisons every external-improvement comparison too.
    //
    // We don't want a single QP divergence to freeze the order book. Fall
    // back to "full-speed planned target with reference-price clearing":
    //   - x  := qH  (each order asks for its full per-batch capacity)
    //   - pi := initial_pi from Init (overlap midpoint or reference price)
    // The allocation phase below will then plan PlannedLegFill entries with
    // finite targets and the three-phase external/internal/external loop
    // clamps to actual feasibility (external curve depth, internal
    // counter-side availability, per-leg price band). So the fallback
    // produces sensible fills exactly when the QP would have, and just
    // avoids the freeze.
    // TODO(F-04): replace SolveImpl with a more stable QP (LDLT with
    // regularization, or a different algorithm). The fallback below should
    // become an alert path, not the default.
    if (!x.allFinite() || !pi.allFinite()) {
        cex::common::log_json(
            "WARN",
            "Solver diverged (NaN) — falling back to qH targets and initial pi",
            {
                {"orders", std::to_string(active_orders.size())},
                {"x_finite", x.allFinite() ? "true" : "false"},
                {"pi_finite", pi.allFinite() ? "true" : "false"},
            });
        x = qH;
        pi = initial_pi;
    }

    // PR-F02-004 dev diagnostic: log raw QP outputs so we can see when x
    // collapses to zero despite active orders on both sides. Cheap (one
    // log per batch), removable when fills land reliably.
    if (!active_orders.empty()) {
        const std::size_t order_count = active_orders.size();
        const Eigen::VectorXd x_orders = x.head(order_count);
        const double x_sum = x_orders.sum();
        const double x_max = order_count > 0 ? x_orders.maxCoeff() : 0.0;
        std::string pi_str;
        for (const auto& [sym, idx] : prices_map) {
            if (!pi_str.empty()) pi_str += ",";
            pi_str += sym + "=" + std::to_string(pi(idx));
        }
        cex::common::log_json(
            "INFO",
            "Solver result raw",
            {
                {"orders", std::to_string(order_count)},
                {"x_sum", std::to_string(x_sum)},
                {"x_max", std::to_string(x_max)},
                {"pi", pi_str},
                {"eps_liquidity", std::to_string(cfg_.epsilon_liquidity)},
                {"tolerance", std::to_string(cfg_.tolerance)},
            });
    }

    fob::matching::v1::BatchResult result;
    std::unordered_map<std::string, SymbolPlan> symbol_plans;
    std::unordered_map<std::string, double> symbol_total_qty;
    std::unordered_map<std::string, double> symbol_total_notional;
    std::unordered_map<std::string, double> symbol_external_consumed;

    // Pre-allocate planned_fills для всех ног заявок.
    size_t total_legs = 0;
    for (const auto& order : active_orders) {
        total_legs += order.legs.size();
    }
    std::vector<PlannedLegFill> planned_fills;
    planned_fills.reserve(total_legs);
    std::vector<double> actual_order_qty(active_orders.size(), 0.0);

    // Записываем начальные clear_prices (= π) в результат. Пересчитаем
    // позже как VWAP по реальным fills.
    for (const auto& [sym, idx] : prices_map) {
        double price = pi(idx);
        (*result.mutable_clear_prices())[sym] = ToDecimal(price).to_proto();
    }

    // Build PlannedLegFill для каждой ноги. Clamp x к [0, qH] чтобы
    // защититься от NaN (хотя выше уже handled).
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

            // C++20 designated initializer — readable struct init.
            planned_fills.push_back(PlannedLegFill{
                .order = &order,
                .order_index = i,
                .symbol = leg.instrument_symbol,
                // BUY = leg.weight > 0, SELL = leg.weight < 0.
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

    // ----- 3-фазное распределение per symbol --------------------------------
    for (auto& [symbol, plan] : symbol_plans) {
        double total_buy = 0.0;
        for (const auto* fill : plan.buys) total_buy += fill->requested_leg_qty;

        double total_sell = 0.0;
        for (const auto* fill : plan.sells) total_sell += fill->requested_leg_qty;

        // Multi-leg shortcut: для multi-leg orders (F-09) используем full
        // requested без external venue (внешние venue не поддерживают
        // multi-leg execution атомарно).
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

        // ----- Local helper closures для allocation phases ------------------
        // update_actual_order_qty — синхронизирует order-level qty (по
        // максимуму ног, scaled by weight_abs).
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

        // ----- allocate_internal_side — pro-rata распределение ОДНОЙ
        // стороны (buys или sells) по их requested_leg_qty. ratio scaled.
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

        // ----- allocate_external_side — распределение через external venue.
        // Учитывает: price band (PriceBandMaxQtyForSingleLegOrder),
        // max tradable (venue depth + limit_price), optional price improvement.
        const auto allocate_external_side = [&](std::vector<PlannedLegFill*>& fills,
                                                const fob::common::v1::Side side,
                                                const double target_total,
                                                const bool require_price_improvement) {
            if (curve_it == external_liquidity.end() || fills.empty() || target_total <= kQtyEps) {
                return 0.0;
            }

            // Уже потреблено на этой стороне (для accumulating q_grid).
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

                // Цена на venue при суммарном qty_for_price.
                const double qty_for_price = consumed + std::min(
                    remaining_target,
                    std::max(0.0, fill->max_leg_qty - fill->actual_leg_qty)
                );
                const double price_for_band = ExternalPriceAtQty(
                    curve_it->second,
                    side,
                    qty_for_price
                ).value_or(fill->clear_price);
                // Сколько может пройти через price band заявки.
                const double price_band_max_qty = PriceBandMaxQtyForSingleLegOrder(
                    *fill->order,
                    side,
                    price_for_band,
                    cfg_.batch_interval
                ) * fill->weight_abs;
                const double remaining_capacity = std::max(
                    0.0,
                    std::min(fill->max_leg_qty, price_band_max_qty) - fill->actual_leg_qty
                );
                if (remaining_capacity <= kQtyEps) {
                    continue;
                }

                // Venue depth limit.
                const double max_tradable = require_price_improvement
                    ? MaxPriceImprovingExternalQtyForOrder(
                          curve_it->second,
                          side,
                          *fill->order,
                          fill->clear_price)
                    : MaxTradableExternalQtyForOrder(curve_it->second, side, *fill->order);
                const double available = std::max(0.0, max_tradable - consumed);
                // Финальный qty = min(target remainder, capacity, available).
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

        // ===== Фаза 1: external IMPROVE ===================================
        // Route to external liquidity first only when its curve improves the
        // internal clearing price. Otherwise local liquidity keeps priority.
        allocate_external_side(
            plan.buys,
            fob::common::v1::SIDE_BUY,
            capacity_remaining(plan.buys),
            true   // require_price_improvement
        );
        allocate_external_side(
            plan.sells,
            fob::common::v1::SIDE_SELL,
            capacity_remaining(plan.sells),
            true
        );

        // ===== Фаза 2: internal CROSS =====================================
        // Match BUY и SELL друг с другом по min(remaining_buy, remaining_sell).
        const double remaining_buy_for_internal = requested_remaining(plan.buys);
        const double remaining_sell_for_internal = requested_remaining(plan.sells);
        const double internal_total =
            std::min(remaining_buy_for_internal, remaining_sell_for_internal);

        allocate_internal_side(plan.buys, internal_total);
        allocate_internal_side(plan.sells, internal_total);

        // ===== Фаза 3: external RESIDUAL ==================================
        // Если одна сторона осталась с unmet demand — допускаем external
        // даже без price improvement (limit price honored).
        const double remaining_buy_after_internal = requested_remaining(plan.buys);
        const double remaining_sell_after_internal = requested_remaining(plan.sells);

        if (remaining_buy_after_internal > remaining_sell_after_internal + kQtyEps) {
            allocate_external_side(
                plan.buys,
                fob::common::v1::SIDE_BUY,
                capacity_remaining(plan.buys),
                false   // no price improvement requirement
            );
        } else if (remaining_sell_after_internal > remaining_buy_after_internal + kQtyEps) {
            allocate_external_side(
                plan.sells,
                fob::common::v1::SIDE_SELL,
                capacity_remaining(plan.sells),
                false
            );
        }

        // Residual norm = max abs(buy-sell mismatch).
        residual_norm = std::max(
            residual_norm,
            std::abs(requested_remaining(plan.buys) - requested_remaining(plan.sells))
        );

        if (symbol_used_external) {
            ++external_symbols_used;
        }
    }

    // ----- Эмиссия fills в BatchResult --------------------------------------
    // Two pass: для каждого PlannedLegFill отдельно эмитим internal и
    // external (если есть). Это даёт downstream раздельную audit-видимость.
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

        // External fill: price берём из venue curve в точке cumulative consumed.
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

    // ----- order_updates для каждой заявки с ненулевым executed_qty --------
    for (size_t i = 0; i < active_orders.size(); ++i) {
        const auto& order = active_orders[i];
        double executed_qty = actual_order_qty[i];
        if (executed_qty <= kQtyEps) {
            continue;   // нет fills — нет update'а
        }

        // rate (units/sec) обратный convert из quantity per interval.
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

    // ----- Перезаписываем clear_prices как VWAP по реальным fills ----------
    for (const auto& [sym, total_qty] : symbol_total_qty) {
        if (total_qty <= kQtyEps) {
            continue;
        }
        (*result.mutable_clear_prices())[sym] =
            ToDecimal(symbol_total_notional[sym] / total_qty).to_proto();
    }

    // ----- diagnostics ------------------------------------------------------
    auto* diag = result.mutable_diagnostics();
    diag->set_residual_norm(residual_norm);
    diag->set_num_active_orders(static_cast<uint32_t>(active_orders.size()));
    diag->set_config_version(cfg_.version);
    // JSON-encoded extra diagnostics — расширяемо без proto-bump'а.
    diag->set_solver_diagnostics_json(
        "{\"kind\":\"continuous_clearing\",\"external_symbols_used\":" +
        std::to_string(external_symbols_used) + "}"
    );

    return result;
}

/// Hot-reload конфига solver'а. Используется control-plane для tuning без
/// рестарта сервиса. config_version в BatchResult позволяет post-trade audit
/// связать batch с конкретным config snapshot'ом.
void ContinuousClearingSolver::SetSolverConfig(SolverConfig cfg) {
    cfg_ = std::move(cfg);
}

}  // namespace cex::matching::domain
