#pragma once
// ============================================================================
// surplus_policy.hpp — F-05A (T-F05A-304). Matching domain. ADR-047 (accepted).
//
// Решение по ненулевому остатку клиринга r = W·x. Чистая логика (header-only):
// на вход — вектор остатка по активам и его норма, политика и tolerance; на выход
// — SurplusDecision (что делать с остатком и применять ли fills).
//
// НЕПЕРЕГОВОРНЫЙ ИНВАРИАНТ (ADR-047 / §17): ledger НИКОГДА не применяет
// несбалансированный ExecutionGroup. Либо остаток в пределах tolerance
// (||r|| ≤ tol ⇒ считаем сбалансированным, fills применяются), либо он ЯВНО
// проводится на house-счёт (EXCHANGE_PNL) / фиксируется отдельной surplus-позицией
// / закрывается MM — иначе группа отклоняется (REJECT_IF_RESIDUAL, prod-default).
//
// Здесь только РЕШЕНИЕ (и квантование остатка в Decimal, §9). Сами ledger-проводки
// и эмиссия proto SurplusEvent — в интеграции batch-loop (T-F05A-305, infra).
// ============================================================================

#include <cmath>    // std::isfinite, std::llround, std::pow
#include <cstdint>
#include <vector>

#include "cex/common/decimal.hpp"

namespace cex::matching::domain {

/// Политика распределения остатка (соответствует proto SurplusAllocationPolicy).
enum class SurplusPolicy {
  kRejectIfResidual,  ///< prod-default: несбалансированную группу НЕ применять
  kExchangePnl,       ///< остаток → house-счёт EXCHANGE_PNL (сохранение баланса)
  kSurplusAsset,      ///< остаток → отдельная surplus-позиция по активу
  kMmLastResort       ///< остаток закрывается MM-ликвидностью (только при лимите)
};

/// Что делать по итогу решения.
enum class SurplusAction {
  kProceedNoSurplus,     ///< ||r|| ≤ tol: сбалансировано, fills применяются
  kReject,               ///< остаток > tol и политика REJECT: группа отклонена
  kAllocateExchangePnl,  ///< house-проводка остатка, fills применяются
  kAllocateSurplusAsset, ///< surplus-позиция, fills применяются
  kAllocateMm            ///< MM-закрытие остатка, fills применяются
};

struct SurplusDecision {
  SurplusAction action{SurplusAction::kReject};
  /// Остаток по активам, квантованный в Decimal (для house-проводки / позиции).
  /// Пуст, если остатка нет или группа отклонена.
  std::vector<cex::common::Decimal> surplus_by_asset;
  /// Применять ли клиринговые fills (x). false только при kReject.
  bool emit_fills{false};
  double residual_norm{0.0};
};

namespace detail {
/// double → Decimal{units, scale} (детерминировано, llround). Граница §9.
inline cex::common::Decimal QuantizeSurplus(double value, std::int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};
  const double factor = std::pow(10.0, static_cast<double>(scale));
  return cex::common::Decimal{static_cast<std::int64_t>(std::llround(value * factor)),
                              scale};
}
}  // namespace detail

/// Решение по остатку. Чистая функция (детерминирована).
///   residual      — r = W·x по активам (double, диагностика §9);
///   residual_norm — ||r||₂;
///   policy        — из solver_config (prod-default kRejectIfResidual);
///   tolerance     — порог «сбалансированности» (тот же, что у солвера).
inline SurplusDecision DecideSurplus(const std::vector<double>& residual,
                                     double residual_norm, SurplusPolicy policy,
                                     double tolerance,
                                     std::int32_t decimal_scale = 12) {
  SurplusDecision d;
  d.residual_norm = residual_norm;

  // Сбалансировано в пределах tolerance — остатка нет, применяем fills.
  if (residual_norm <= tolerance) {
    d.action = SurplusAction::kProceedNoSurplus;
    d.emit_fills = true;
    return d;
  }

  // Остаток значим — по политике.
  switch (policy) {
    case SurplusPolicy::kRejectIfResidual:
      d.action = SurplusAction::kReject;
      d.emit_fills = false;  // несбалансированную группу НЕ применяем (§17)
      return d;
    case SurplusPolicy::kExchangePnl:
      d.action = SurplusAction::kAllocateExchangePnl;
      break;
    case SurplusPolicy::kSurplusAsset:
      d.action = SurplusAction::kAllocateSurplusAsset;
      break;
    case SurplusPolicy::kMmLastResort:
      d.action = SurplusAction::kAllocateMm;
      break;
  }
  // Для аллокационных политик: остаток проводится явно, fills применяются.
  d.emit_fills = true;
  d.surplus_by_asset.reserve(residual.size());
  for (double r : residual) {
    d.surplus_by_asset.push_back(detail::QuantizeSurplus(r, decimal_scale));
  }
  return d;
}

}  // namespace cex::matching::domain
