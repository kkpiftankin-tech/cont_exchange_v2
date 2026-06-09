// ============================================================================
// grouped_solver_bisection.cpp — F-09 (T-F09-044). См. .hpp.
//
// Алгоритм (MVP-2, ratio/basket):
//   ρ_i = |target_ratio|; Q_i = feasible cap; rem_i = remaining_qty.
//   G_feasible = min_i (Q_i / ρ_i)      — макс. group unit с учётом всех капов.
//   G_desired  = min_i (rem_i / ρ_i)    — желаемый group unit (только remaining).
//   α          = clamp_[0,1]( G_feasible / G_desired ).
//   e_i        = min( G_exec · ρ_i, Q_i ).
// Политики:
//   strict_atomic   — исполнить только если G_feasible == G_desired (α = 1), иначе 0.
//   scalable_atomic — исполнить G_feasible, если α ≥ min_execution_scale, иначе 0.
//   best_effort     — каждая нога по своему Q_i; при непропорциональности — degraded.
// ============================================================================

#include "domain/grouped_solver_bisection.hpp"

#include <string>
#include <unordered_map>

namespace cex::matching::domain {

namespace {

using cex::common::Decimal;

constexpr std::int32_t kGroupScale = 12;  // scale для G = Q/ρ
constexpr std::int32_t kAlphaScale = 6;   // scale для α

const Decimal kZero = Decimal::zero();
const Decimal kOne = Decimal{1, 0};

Decimal Clamp01(const Decimal& a) {
  if (Decimal::cmp(a, kZero) < 0) return kZero;
  if (Decimal::cmp(a, kOne) > 0) return kOne;
  return a;
}

std::string FallbackActionStr(GroupFallbackPolicy p) {
  switch (p) {
    case GroupFallbackPolicy::kScaleDown: return "scale_down";
    case GroupFallbackPolicy::kWaitNextBatch: return "wait_next_batch";
    case GroupFallbackPolicy::kCancel: return "cancel";
    case GroupFallbackPolicy::kDegrade: return "degrade";
    case GroupFallbackPolicy::kCompensate: return "compensate";
    default: return "scale_down";
  }
}

}  // namespace

GroupedSolveResult GroupedSolverBisection::Solve(const GroupedSolveInput& input) {
  const auto& order = input.order;
  GroupedSolveResult res;
  res.execution_scale = kZero;

  // symbol → feasible cap.
  std::unordered_map<std::string, Decimal> cap_by_sym;
  cap_by_sym.reserve(input.feasible_caps.size());
  for (const auto& fc : input.feasible_caps) {
    cap_by_sym[fc.instrument_symbol] = fc.cap;
  }
  const auto cap_of = [&](const VectorLeg& leg) -> Decimal {
    const auto it = cap_by_sym.find(leg.instrument_symbol);
    return it != cap_by_sym.end() ? it->second : leg.remaining_qty();
  };

  // G_feasible = min(Q_i/ρ_i); G_desired = min(rem_i/ρ_i); G_feas_max — для best_effort.
  bool first = true;
  Decimal g_feas = kZero, g_des = kZero, g_feas_max = kZero;
  for (const auto& leg : order.legs) {
    const Decimal rho = leg.abs_ratio();
    if (Decimal::cmp(rho, kZero) == 0) continue;  // нога без коэффициента — пропуск
    const Decimal gi = Decimal::div(cap_of(leg), rho, kGroupScale);
    const Decimal di = Decimal::div(leg.remaining_qty(), rho, kGroupScale);
    if (first) {
      g_feas = gi;
      g_feas_max = gi;
      g_des = di;
      res.binding_leg = leg.instrument_symbol;
      first = false;
    } else {
      if (Decimal::cmp(gi, g_feas) < 0) {
        g_feas = gi;
        res.binding_leg = leg.instrument_symbol;
      }
      if (Decimal::cmp(gi, g_feas_max) > 0) g_feas_max = gi;
      if (Decimal::cmp(di, g_des) < 0) g_des = di;
    }
  }

  if (first) {  // нет валидных ног
    res.status = GroupExecStatus::kBlocked;
    return res;
  }

  // α = clamp_[0,1](G_feasible / G_desired).
  Decimal alpha = kZero;
  if (Decimal::cmp(g_des, kZero) > 0) {
    alpha = Clamp01(Decimal::div(g_feas, g_des, kAlphaScale));
  }
  res.execution_scale = alpha;

  // best_effort: каждая нога по своему капу, без ratio-lock.
  if (order.atomicity_policy == GroupAtomicityPolicy::kBestEffort) {
    for (const auto& leg : order.legs) {
      if (Decimal::cmp(leg.abs_ratio(), kZero) == 0) continue;
      res.leg_execs.push_back(
          LegExec{leg.leg_id, leg.instrument_symbol, cap_of(leg), leg.target_ratio});
    }
    // Непропорциональность капов → нарушение соотношения.
    if (Decimal::cmp(g_feas, g_feas_max) != 0) {
      res.violated_constraints.push_back("ratio_deviation");
      res.status = GroupExecStatus::kDegraded;
    } else {
      res.status = GroupExecStatus::kFullyExecuted;
    }
    return res;
  }

  // strict / scalable: общий масштаб G_exec.
  Decimal g_exec = kZero;
  switch (order.atomicity_policy) {
    case GroupAtomicityPolicy::kStrictAtomic:
      if (Decimal::cmp(g_feas, g_des) < 0) {
        res.status = GroupExecStatus::kBlocked;  // не атомарно → 0
      } else {
        g_exec = g_feas;
        res.status = GroupExecStatus::kFullyExecuted;
        res.execution_scale = kOne;
      }
      break;
    case GroupAtomicityPolicy::kScalableAtomic:
      if (Decimal::cmp(alpha, order.min_execution_scale) < 0) {
        res.status = GroupExecStatus::kBlocked;  // α < α_min → 0
      } else {
        g_exec = g_feas;
        res.status = Decimal::cmp(alpha, kOne) >= 0 ? GroupExecStatus::kFullyExecuted
                                                    : GroupExecStatus::kScaled;
      }
      break;
    default:
      // sequential_fallback / external_compensating — MVP-4/5.
      res.status = GroupExecStatus::kBlocked;
      res.violated_constraints.push_back("policy_not_implemented_mvp2");
      break;
  }

  if (Decimal::cmp(g_exec, kZero) > 0) {
    for (const auto& leg : order.legs) {
      const Decimal rho = leg.abs_ratio();
      if (Decimal::cmp(rho, kZero) == 0) continue;
      // e_i = G_exec · ρ_i, зажато капом (страховка от округления).
      Decimal e = Decimal::min(Decimal::mul(g_exec, rho), cap_of(leg));
      res.leg_execs.push_back(LegExec{leg.leg_id, leg.instrument_symbol, e, leg.target_ratio});
    }
  } else {
    // Блокировка → ничего не исполнено (scale=0) + fallback action из политики.
    res.execution_scale = kZero;
    res.fallback_action = FallbackActionStr(order.fallback_policy);
  }

  return res;
}

}  // namespace cex::matching::domain
