#pragma once
// =============================================================================
// RiskUseCases — application-логика risk-сервиса.
//
// Хранит лёгкое in-memory состояние kill-switch'а (глобального и по инструменту).
// Все остальные проверки сейчас вычисляются на каждый запрос (stateless),
// что в будущем можно расширять (PnL пользователя, открытые позиции, лимиты).
// =============================================================================

#include <mutex>
#include <string>
#include <unordered_map>

#include "fob/risk/v1/risk.pb.h"
#include "infra/risk_alerts_publisher.hpp"

namespace cex::risk::app {

class RiskUseCases {
 public:
  // publisher двигается внутрь — use-case владеет паблишером алертов.
  explicit RiskUseCases(infra::RiskAlertsPublisher publisher);

  // Pre-trade проверка ордера. Решения: ACCEPT / REJECT / HALT (kill-switch).
  fob::risk::v1::PreTradeCheckResponse CheckNewOrder(
      const fob::risk::v1::PreTradeCheckRequest& req);

  // Установить/снять kill switch. Если instrument_symbol пустой — глобальный halt.
  // Любая мутация дополнительно эмиттит RiskAlert в Kafka.
  fob::risk::v1::KillSwitchResponse SetKillSwitch(
      const fob::risk::v1::KillSwitchRequest& req);

  // Post-trade обновление от matching'а (через gRPC или future Kafka). Сейчас
  // только логирует диагностику; в будущем — апдейт PnL/exposure пользователей.
  void OnBatchResult(const fob::risk::v1::PostTradeUpdateRequest& req);

 private:
  // Должен вызываться под захваченным mu_ — отсюда суффикс _locked.
  bool is_halted_locked(const std::string& symbol) const;

  // mutable — для const-геттеров.
  mutable std::mutex mu_;
  bool global_halt_{false};                                  // глобальный halt
  std::unordered_map<std::string, bool> instrument_halt_;    // halt по symbol

  infra::RiskAlertsPublisher publisher_;
};

}  // namespace cex::risk::app
