#pragma once
// ============================================================================
// auto_resolve_loop.hpp — F-09 MVP-7 (ADR-041). Авто-резолв combo-компенсаций.
//
// Периодический поток (gated на config.enabled, default OFF). На каждом проходе:
// ListPending → для каждой кандидат (reason/age/notional) → pure AutoResolvePolicy
// → при kAutoReverse вызывает ResolveCompensationUseCase(reverse_internal,
// operator_id="auto:..."). Тот же money-путь, что у оператора (ADR-040) — никакого
// нового. Скользящее окно (notional/count) — circuit-breaker (ADR-041 §3).
// ============================================================================

#include <atomic>
#include <cstdint>
#include <deque>
#include <thread>

#include "app/auto_resolve_policy.hpp"
#include "app/resolve_compensation_use_case.hpp"
#include "infra/matching_compensation_client.hpp"
#include "infra/postgres_combo_order_repository.hpp"

namespace cex::order_flow::app {

class AutoResolveLoop {
 public:
  AutoResolveLoop(infra::MatchingCompensationClient* comp_client,
                  infra::IComboOrderRepository* combo_repo,
                  ResolveCompensationUseCase* resolve_uc,
                  AutoResolveConfig config,
                  std::int64_t interval_ms,
                  std::int64_t window_ms);
  ~AutoResolveLoop();

  void Start();
  void Stop();

  // Один проход (для тестов/ручного вызова). Возвращает число авто-резолвов.
  int RunOnce(std::int64_t now_ms);

 private:
  void PruneWindow(std::int64_t now_ms);
  AutoResolveWindow WindowSnapshot() const;

  infra::MatchingCompensationClient* comp_client_;
  infra::IComboOrderRepository* combo_repo_;
  ResolveCompensationUseCase* resolve_uc_;
  AutoResolveConfig config_;
  std::int64_t interval_ms_;
  std::int64_t window_ms_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  // Скользящее окно: (timestamp_ms, notional) применённых авто-резолвов.
  std::deque<std::pair<std::int64_t, cex::common::Decimal>> window_;
};

}  // namespace cex::order_flow::app
