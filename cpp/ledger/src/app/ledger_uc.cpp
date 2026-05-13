// =============================================================================
// Реализация LedgerUseCases. Все мутации балансов проходят под mu_, чтобы
// gRPC-запросы и Kafka-консьюмеры не наступали друг другу на состояние.
// =============================================================================

#include "app/ledger_uc.hpp"

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::ledger::app {

using cex::common::Decimal;

LedgerUseCases::LedgerUseCases() {
  // Демо-баланс, чтобы можно было сразу выставлять ордера в dev-стенде:
  //   demo-user: 10 000 USDT (scale=2) и 1 BTC (scale=8).
  std::lock_guard<std::mutex> lg(mu_);

  balances_["demo-user"]["USDT"].available = Decimal{10'00000, 2}; // 10000.00 USDT
  balances_["demo-user"]["USDT"].reserved  = Decimal{0, 2};

  balances_["demo-user"]["BTC"].available = Decimal{1'00000000, 8}; // 1.00000000 BTC
  balances_["demo-user"]["BTC"].reserved  = Decimal{0, 8};
}

// Возвращает ссылку на Balance, создавая запись при первом обращении.
// Должен вызываться ТОЛЬКО под захваченным mu_.
LedgerUseCases::Balance& LedgerUseCases::ensure_balance_locked(
    const std::string& user, const std::string& currency) {
  auto& ub = balances_[user];
  auto& b = ub[currency];
  return b;
}

// GET /balances — формируем ответ из текущего состояния.
fob::ledger::v1::GetBalancesResponse LedgerUseCases::GetBalances(
    const fob::ledger::v1::GetBalancesRequest& req) {
  fob::ledger::v1::GetBalancesResponse resp;
  // Эхо мета-данные запроса + проставляем себя как источник.
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  auto it_user = balances_.find(req.user_id());
  // Неизвестный пользователь — это просто пустой ответ (а не ошибка).
  if (it_user == balances_.end()) return resp;

  for (const auto& [ccy, bal] : it_user->second) {
    auto* out = resp.add_balances();
    out->set_currency(ccy);
    *out->mutable_available() = bal.available.to_proto();
    *out->mutable_reserved()  = bal.reserved.to_proto();
    *out->mutable_total()     = Decimal::add(bal.available, bal.reserved).to_proto();
  }
  return resp;
}

// Резерв средств под открытие ордера.
// Идемпотентность: повторный запрос с тем же reservation_id вернёт success
// и текущую сумму, не изменяя баланс. Это важно для at-least-once ретраев.
fob::ledger::v1::ReserveFundsResponse LedgerUseCases::ReserveFunds(
    const fob::ledger::v1::ReserveFundsRequest& req) {
  fob::ledger::v1::ReserveFundsResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);

  // Идемпотентность по reservation_id (MVP: просто проверка наличия).
  if (reservations_.count(req.reservation_id())) {
    resp.set_success(true);
    *resp.mutable_reserved_amount() = reservations_[req.reservation_id()].amount.to_proto();
    return resp;
  }

  auto& b = ensure_balance_locked(req.user_id(), req.currency());

  Decimal want = Decimal::from_proto(req.amount());
  // Сравнение делает выравнивание scale внутри Decimal::cmp.
  if (Decimal::cmp(b.available, want) < 0) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INSUFFICIENT_FUNDS");
    e->set_message("Not enough available balance to reserve.");
    return resp;
  }

  // Перенос: available -= want, reserved += want.
  b.available = Decimal::sub(b.available, want);
  b.reserved  = Decimal::add(b.reserved,  want);

  // Сохраняем мета о резервации, чтобы потом корректно её снять.
  reservations_[req.reservation_id()] = Reservation{
      .user_id=req.user_id(),
      .order_id=req.order_id(),
      .currency=req.currency(),
      .amount=want,
  };

  resp.set_success(true);
  *resp.mutable_reserved_amount() = want.to_proto();

  cex::common::log_json("INFO", "Reserved funds",
                        {{"user", req.user_id()},
                         {"currency", req.currency()},
                         {"amount", want.to_string()},
                         {"reservation_id", req.reservation_id()}});
  return resp;
}

// Освобождение резерва (например, при cancel ордера или несоответствии в матчинге).
// На неизвестный id отвечаем WARN-логом, но не падаем — это безопасный no-op.
void LedgerUseCases::ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req) {
  std::lock_guard<std::mutex> lg(mu_);

  auto it = reservations_.find(req.reservation_id());
  if (it == reservations_.end()) {
    cex::common::log_json("WARN", "ReleaseFunds: reservation not found",
                          {{"reservation_id", req.reservation_id()}});
    return;
  }

  // Обратный перенос: reserved -= amount, available += amount.
  auto& b = ensure_balance_locked(it->second.user_id, it->second.currency);
  b.reserved  = Decimal::sub(b.reserved,  it->second.amount);
  b.available = Decimal::add(b.available, it->second.amount);

  cex::common::log_json("INFO", "Released funds",
                        {{"user", it->second.user_id},
                         {"currency", it->second.currency},
                         {"amount", it->second.amount.to_string()},
                         {"reservation_id", req.reservation_id()}});
  reservations_.erase(it);
}

// Применение результата матчинга — фактическое изменение балансов после fill'ов.
// Логика на пальцах:
//   * BUY  : пользователь "тратит" зарезервированную котируемую валюту (quote)
//            и получает базовую (base) на available.
//   * SELL : наоборот — списываем base из reserved, добавляем quote в available.
fob::ledger::v1::ApplyBatchResultResponse LedgerUseCases::ApplyBatchResult(
    const fob::ledger::v1::ApplyBatchResultRequest& req) {
  fob::ledger::v1::ApplyBatchResultResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);

  for (const auto& fill : req.batch().fills()) {
    const std::string user  = fill.user_id();
    const std::string base  = fill.instrument().base();
    const std::string quote = fill.instrument().quote();

    Decimal qty      = Decimal::from_proto(fill.executed_qty());
    Decimal notional = Decimal::from_proto(fill.executed_notional());

    if (fill.side() == fob::common::v1::SIDE_BUY) {
      // Покупка: квота уходит из reserved (она была зарезервирована при ReserveFunds).
      auto& qbal = ensure_balance_locked(user, quote);
      qbal.reserved = Decimal::sub(qbal.reserved, notional);

      // Базовая валюта поступает в available.
      auto& bbal = ensure_balance_locked(user, base);
      bbal.available = Decimal::add(bbal.available, qty);
    } else if (fill.side() == fob::common::v1::SIDE_SELL) {
      // Продажа: списываем base из reserved.
      auto& bbal = ensure_balance_locked(user, base);
      bbal.reserved = Decimal::sub(bbal.reserved, qty);

      // Получаем quote в available.
      auto& qbal = ensure_balance_locked(user, quote);
      qbal.available = Decimal::add(qbal.available, notional);
    }
  }

  resp.set_success(true);
  return resp;
}

// Применение execution report — отчёт о хедж-операции на внешней площадке.
// В MVP не делаем учёт хеджей, просто пишем INFO-лог.
void LedgerUseCases::ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req) {
  cex::common::log_json("INFO", "ApplyExecutionReport",
                        {{"venue", req.report().venue()},
                         {"intent_id", req.report().intent_id()},
                         {"status", std::to_string(req.report().status())}});
}

}  // namespace cex::ledger::app
