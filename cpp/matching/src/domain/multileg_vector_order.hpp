#pragma once
// ============================================================================
// multileg_vector_order.hpp — F-09 (T-F09-043). Matching domain.
//
// MultiLegVectorOrder — представление combo-группы для grouped vector solver:
// набор ног со знаковым целевым коэффициентом ρ_g (target_ratio), ценовыми
// диапазонами, скоростью/объёмом и внешними кап-ограничениями (ликвидность,
// риск, venue). Чистая модель (без proto/Kafka/DB) — solver работает с ней.
//
// MVP-2: ratio/basket, per-symbol bisection. Матрицы ограничений A_g/G_g
// (spread/factor) — MVP-3 (QP), здесь не моделируются.
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::matching::domain {

/// Политика атомарности группы (зеркало AtomicityPolicy из combo.proto).
enum class GroupAtomicityPolicy {
  kStrictAtomic,        // всё-или-ничего: α < α_min → группа не исполняется
  kScalableAtomic,      // исполняется пропорциональная доля α по всем ногам
  kBestEffort,          // максимум исполнения; нарушения фиксируются
  kSequentialFallback,  // последовательное исполнение (MVP-4)
  kExternalCompensating // внешняя компенсация (MVP-5)
};

/// Политика отката при деградации (зеркало fallback_policy).
enum class GroupFallbackPolicy {
  kScaleDown,     // уменьшить масштаб
  kWaitNextBatch, // пропустить batch, ждать следующий
  kCancel,        // отменить группу
  kDegrade,       // исполнить с нарушением мягких ограничений
  kCompensate     // компенсирующая транзакция
};

/// Нога группы для векторного солвера.
struct VectorLeg {
  std::string instrument_symbol;

  /// Знаковый целевой коэффициент ρ_g (signed): >0 — buy base, <0 — sell base.
  /// Для ratio_basis=NOTIONAL_WEIGHT — вес со знаком; для QUANTITY — ratio.
  cex::common::Decimal target_ratio{};

  cex::common::Decimal p_low{};
  cex::common::Decimal p_high{};

  /// Per-batch кап по объёму из ограничения скорости (speed × dt) — интегрирование
  /// скорости по окну батча делает вызывающий use case; здесь это уже base-qty.
  cex::common::Decimal q_rate{};
  cex::common::Decimal q_max{};
  cex::common::Decimal filled_cum{};

  /// Внешние капы (base units), заполняются use case из market data / risk / venue.
  /// nullopt трактуется как +∞ (не ограничивает).
  std::optional<cex::common::Decimal> q_liq;
  std::optional<cex::common::Decimal> q_risk;
  std::optional<cex::common::Decimal> q_venue;

  /// Оставшийся объём ноги (q_max - filled_cum, не отрицательный).
  [[nodiscard]] cex::common::Decimal remaining_qty() const {
    const auto remaining = cex::common::Decimal::sub(q_max, filled_cum);
    const cex::common::Decimal zero{0, remaining.scale};
    return cex::common::Decimal::cmp(remaining, zero) < 0 ? zero : remaining;
  }

  /// |target_ratio| — абсолютная величина целевого коэффициента.
  [[nodiscard]] cex::common::Decimal abs_ratio() const {
    const cex::common::Decimal zero{0, target_ratio.scale};
    return cex::common::Decimal::cmp(target_ratio, zero) < 0
               ? cex::common::Decimal::sub(zero, target_ratio)
               : target_ratio;
  }
};

/// Группа целиком.
struct MultiLegVectorOrder {
  std::string parent_order_id;
  std::vector<VectorLeg> legs;

  GroupAtomicityPolicy atomicity_policy{GroupAtomicityPolicy::kBestEffort};
  GroupFallbackPolicy fallback_policy{GroupFallbackPolicy::kScaleDown};

  /// Минимально допустимый масштаб α_min ∈ [0,1]. Для scalable_atomic: группа не
  /// исполняется, если достижимый α < α_min. Для strict_atomic трактуется как 1.
  cex::common::Decimal min_execution_scale{};

  /// Максимально допустимое отклонение соотношения (bps), для scalable.
  std::int32_t max_ratio_deviation_bps{0};
};

/// Feasible cap одной ноги: Q_feasible = min(remaining, rate, liq, risk, venue).
struct FeasibleCap {
  std::string instrument_symbol;
  cex::common::Decimal cap{};
  /// Связывающий фактор (диагностика): "remaining"|"rate"|"liq"|"risk"|"venue".
  std::string binding_factor;
};

}  // namespace cex::matching::domain
