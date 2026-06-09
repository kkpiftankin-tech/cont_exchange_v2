// ============================================================================
// hedge_trigger_policy.cpp — F-12 trigger evaluation (PR-F12-5).
//
// Назначение и физический смысл:
//   После каждого batch matching формирует position snapshots — суммарный
//   net_qty по symbol после применения fills. Если net |net_qty| превысил
//   threshold (по qty или по notional = qty * reference_mid), необходимо
//   эмиттить hedge через external venue.
//
//   Эта policy НЕ строит hedge intent — только решает "нужно ли хеджировать
//   и почему". Сам intent строит ExecutionIntentBuilder (см.
//   cpp/matching/src/app/execution_intent_builder.cpp).
//
// Конфигурация (PR-F12-5, см. .env-example HEDGE_TRIGGER_*):
//   - threshold_qty       — порог по абсолютному net qty (base units).
//   - threshold_notional  — порог по notional (quote units = qty * mid).
//   - 0 в любом — disabled (та проверка не работает).
//
// Per-symbol overrides:
//   HEDGE_TRIGGER_SYMBOLS=BTC/USDT,ETH/USDT + HEDGE_TRIGGER_QTY_BTC_USDT=0.001
//   позволяют тонкую настройку. Default — общий fallback.
//
// Контракт:
//   triggered = qty_exceeded OR notional_exceeded. Хеджируется любой из них.
//   Disabled-проверка считается не trigger'нутой.
// ============================================================================

#include "app/hedge_trigger_policy.hpp"

#include <utility>

namespace cex::matching::app {

namespace {

// calculate abs v
/// |value| через Decimal::sub(zero, value) для negative — без работы с
/// internal units (защищает от int64_min UB-зоны).
cex::common::Decimal AbsDecimal(const cex::common::Decimal& value) {
  if (cex::common::Decimal::cmp(value, cex::common::Decimal::zero()) >= 0) {
    return value;
  }
  return cex::common::Decimal::sub(cex::common::Decimal::zero(), value);
}

/// Threshold "включён" если > 0. 0 (или отрицательный) → проверка
/// отключена. Семантика "0 = disabled" зафиксирована в .env-example.
bool IsThresholdEnabled(const cex::common::Decimal& threshold) {
  return cex::common::Decimal::cmp(threshold, cex::common::Decimal::zero()) > 0;
}

/// Reference mid (clearing price из BatchResult) должен быть строго > 0 для
/// корректного notional. 0 → market data не пришла → skip notional check.
bool HasValidReferenceMid(const fob::common::v1::Decimal& clearing_price) {
  const auto reference_mid = cex::common::Decimal::from_proto(clearing_price);
  return cex::common::Decimal::cmp(reference_mid, cex::common::Decimal::zero()) > 0;
}

}  // namespace

HedgeTriggerPolicy::HedgeTriggerPolicy(HedgeTriggerConfig config)
    : config_(std::move(config)) {}

// ============================================================================
// Evaluate — главная логика: положения → решения о hedge.
//
// Для каждого PositionSnapshot:
//   1. abs_qty = |net_qty|.
//   2. Resolve threshold (per-symbol override или default).
//   3. Check qty: abs_qty > threshold_qty (если qty-check enabled).
//   4. Check notional: abs_qty * reference_mid > threshold_notional (если
//      notional-check enabled и reference_mid > 0).
//   5. triggered = qty OR notional.
//
// Возвращает один HedgeTriggerDecision на snapshot (даже если не triggered —
// для observability и downstream debugging).
// ============================================================================
std::vector<HedgeTriggerDecision> HedgeTriggerPolicy::Evaluate(
    const std::vector<PositionSnapshot>& snapshots) const {
  std::vector<HedgeTriggerDecision> decisions;
  decisions.reserve(snapshots.size());

  for (const auto& snapshot : snapshots) {
    HedgeTriggerDecision decision;
    decision.snapshot = snapshot;
    decision.abs_qty = AbsDecimal(snapshot.net_qty);

    const auto& thresholds = ResolveThresholdForSymbol(snapshot.symbol);
    decision.qty_check_enabled = IsThresholdEnabled(thresholds.threshold_qty);
    decision.notional_check_enabled =
        IsThresholdEnabled(thresholds.threshold_notional) &&
        HasValidReferenceMid(snapshot.clearing_price);

    if (decision.qty_check_enabled) {
      decision.qty_threshold_exceeded =
          cex::common::Decimal::cmp(decision.abs_qty, thresholds.threshold_qty) > 0;
    }

    if (decision.notional_check_enabled) {
      const auto reference_mid = cex::common::Decimal::from_proto(snapshot.clearing_price);
      decision.abs_notional = cex::common::Decimal::mul(decision.abs_qty, reference_mid);
      decision.notional_threshold_exceeded =
          cex::common::Decimal::cmp(decision.abs_notional,
                                    thresholds.threshold_notional) > 0;
    }

    // OR-логика: достаточно одного нарушенного threshold чтобы trigger.
    decision.triggered = decision.qty_threshold_exceeded ||
                         decision.notional_threshold_exceeded;
    decisions.push_back(std::move(decision));
  }

  return decisions;
}

/// Empty config — все threshold = 0 = disabled. Используется в тестах и
/// pre-PR-F12-5 backward compat (когда env не задан → policy noop).
HedgeTriggerConfig HedgeTriggerPolicy::DefaultConfig() {
  return {};
}

/// Per-symbol lookup с fallback на default. unordered_map::find — O(1)
/// average. Возвращает const&, безопасно ссылаться на config_.
const HedgeTriggerThreshold& HedgeTriggerPolicy::ResolveThresholdForSymbol(
    const std::string& symbol) const {
  const auto it = config_.per_symbol_thresholds.find(symbol);
  if (it != config_.per_symbol_thresholds.end()) {
    return it->second;
  }
  return config_.default_thresholds;
}

}  // namespace cex::matching::app
