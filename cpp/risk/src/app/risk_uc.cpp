// =============================================================================
// Реализация RiskUseCases. Минимальный набор pre-trade чеков + kill switch.
// =============================================================================

#include "app/risk_uc.hpp"

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "cex/common/decimal.hpp"

namespace cex::risk::app {

using cex::common::Decimal;

RiskUseCases::RiskUseCases(infra::RiskAlertsPublisher publisher)
    : publisher_(std::move(publisher)) {}

// Утилита: текущий halt-статус для инструмента (учитывает и global_halt_).
// Должна вызываться под захваченным mu_.
bool RiskUseCases::is_halted_locked(const std::string& symbol) const {
  if (global_halt_) return true;
  auto it = instrument_halt_.find(symbol);
  if (it == instrument_halt_.end()) return false;
  return it->second;
}

// Pre-trade проверка нового ордера. Возвращает Decision: ACCEPT/REJECT/HALT.
// При ACCEPT также прикладывает оценку начальной маржи (placeholder).
fob::risk::v1::PreTradeCheckResponse RiskUseCases::CheckNewOrder(
    const fob::risk::v1::PreTradeCheckRequest& req) {
  fob::risk::v1::PreTradeCheckResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("risk");

  // 1) Kill switch — самая первая и самая жёсткая проверка.
  {
    std::lock_guard<std::mutex> lg(mu_);
    if (is_halted_locked(req.order().instrument().symbol())) {
      resp.set_decision(fob::risk::v1::RISK_DECISION_HALT);
      auto* e = resp.mutable_error();
      e->set_code("KILL_SWITCH");
      e->set_message("Trading is halted for this instrument (or globally).");
      return resp;
    }
  }

  // 2) Базовые sanity-чеки.
  if (req.order().total_qty().units() <= 0) {
    resp.set_decision(fob::risk::v1::RISK_DECISION_REJECT);
    auto* e = resp.mutable_error();
    e->set_code("BAD_QTY");
    e->set_message("total_qty must be > 0");
    return resp;
  }

  if (Decimal::cmp(Decimal::from_proto(req.order().price_low()),
                   Decimal::from_proto(req.order().price_high())) > 0) {
    resp.set_decision(fob::risk::v1::RISK_DECISION_REJECT);
    auto* e = resp.mutable_error();
    e->set_code("BAD_PRICE_RANGE");
    e->set_message("price_low must be <= price_high");
    return resp;
  }

  // 3) Очень упрощённая оценка начальной маржи: 10% от условной стоимости.
  //    notional ≈ total_qty * reference_price; margin = notional * 0.1.
  //    Реальная формула зависит от типа инструмента и режима маржи.
  Decimal qty = Decimal::from_proto(req.order().total_qty());
  Decimal ref = Decimal::from_proto(req.reference_price());
  Decimal notional = Decimal::mul(qty, ref);

  Decimal margin = notional;
  margin.units /= 10; // *0.1, scale сохраняем
  *resp.mutable_required_initial_margin() = margin.to_proto();

  resp.set_decision(fob::risk::v1::RISK_DECISION_ACCEPT);
  return resp;
}

// Включение/выключение kill-switch + публикация алерта.
fob::risk::v1::KillSwitchResponse RiskUseCases::SetKillSwitch(
    const fob::risk::v1::KillSwitchRequest& req) {
  fob::risk::v1::KillSwitchResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("risk");

  // 1) Обновление состояния.
  {
    std::lock_guard<std::mutex> lg(mu_);
    if (req.instrument_symbol().empty()) {
      // Пустой symbol = глобальный halt.
      global_halt_ = req.halt();
      resp.set_effective_halt(global_halt_);
    } else {
      instrument_halt_[req.instrument_symbol()] = req.halt();
      resp.set_effective_halt(req.halt());
    }
  }

  // 2) Публикуем алерт — observability/operator UI должен показать его сразу.
  fob::risk::v1::RiskAlert alert;
  auto* meta = alert.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("risk");
  meta->set_correlation_id(req.meta().correlation_id());
  // Если symbol пустой — относим алерт к "GLOBAL" партиции.
  meta->set_partition_key(req.instrument_symbol().empty() ? "GLOBAL" : req.instrument_symbol());

  alert.set_alert_id(cex::common::uuid_v4());
  alert.set_severity(req.halt() ? fob::risk::v1::RISK_SEVERITY_CRITICAL
                                : fob::risk::v1::RISK_SEVERITY_INFO);
  alert.set_user_id(""); // действие оператора, конкретного пользователя нет
  alert.set_instrument_symbol(req.instrument_symbol());
  alert.set_alert_type("KILL_SWITCH");
  auto* e = alert.mutable_error();
  e->set_code(req.halt() ? "HALT" : "RESUME");
  e->set_message(req.reason());
  *alert.mutable_timestamp() = cex::common::now_ts();

  publisher_.publish(alert);

  return resp;
}

// Post-trade обновление: на сейчас просто лог диагностики солвера. В будущем —
// апдейт открытых позиций пользователей и пересчёт лимитов.
void RiskUseCases::OnBatchResult(const fob::risk::v1::PostTradeUpdateRequest& req) {
  const auto& d = req.batch().diagnostics();
  cex::common::log_json("INFO", "OnBatchResult",
                        {{"batch_id", req.batch().batch_id()},
                         {"residual_norm", std::to_string(d.residual_norm())},
                         {"solve_ms", std::to_string(d.solve_time_ms())}});
}

}  // namespace cex::risk::app
