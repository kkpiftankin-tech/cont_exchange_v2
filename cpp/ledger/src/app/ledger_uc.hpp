#pragma once
// =============================================================================
// LedgerUseCases — application-слой сервиса ledger.
//
// В этой версии (MVP) хранение полностью in-memory:
//   * balances_      — балансы пользователя по валютам;
//   * reservations_  — активные резервы под открытые ордера.
// Для прода нужен event-sourced подход с надёжным хранилищем (например,
// Postgres + WAL или собственный append-only лог), но текущая реализация
// достаточна, чтобы продемонстрировать поток данных и контракт.
//
// Мьютекс mu_ защищает оба map'а целиком (грубая, но безопасная гранулярность).
// =============================================================================

#include <mutex>
#include <string>
#include <unordered_map>

#include "cex/common/decimal.hpp"
#include "fob/ledger/v1/ledger.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/execution/v1/execution.pb.h"

namespace cex::ledger::app {

class LedgerUseCases {
 public:
  // Демо-балансы создаются прямо в конструкторе для быстрого старта в dev.
  LedgerUseCases();

  // Запрос балансов пользователя — возвращает available/reserved/total по валютам.
  fob::ledger::v1::GetBalancesResponse GetBalances(const fob::ledger::v1::GetBalancesRequest& req);

  // Резервирование средств под ордер. Идемпотентно по reservation_id.
  // При недостатке средств возвращает success=false и код "INSUFFICIENT_FUNDS".
  fob::ledger::v1::ReserveFundsResponse ReserveFunds(const fob::ledger::v1::ReserveFundsRequest& req);

  // Снять резерв (например, при отмене ордера). По неизвестному id — no-op + WARN.
  void ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req);

  // Применить результат батча матчинга к балансам пользователей.
  // Алгоритм: для каждого fill — уменьшить reserved списываемой валюты,
  // увеличить available получаемой валюты (см. реализацию).
  fob::ledger::v1::ApplyBatchResultResponse ApplyBatchResult(
      const fob::ledger::v1::ApplyBatchResultRequest& req);

  // Применить execution-репорт от внешнего venue (хедж-операции).
  // В MVP только логирует — реальная хедж-бухгалтерия не реализована.
  void ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req);

 private:
  // Внутренние структуры: разделяем баланс на доступный и зарезервированный,
  // чтобы независимо работать с каждой суммой при матчинге и отмене ордеров.
  struct Balance {
    cex::common::Decimal available;
    cex::common::Decimal reserved;
  };

  // Резерв с привязкой к пользователю/ордеру/валюте — нужно для корректного
  // ReleaseFunds (по reservation_id мы должны вернуть именно эту сумму
  // именно этому пользователю).
  struct Reservation {
    std::string user_id;
    std::string order_id;
    std::string currency;
    cex::common::Decimal amount;
  };

  // Тип-алиас для удобства: пользователь -> валюта -> Balance.
  using UserBalances = std::unordered_map<std::string, Balance>;

  // mutable, чтобы const-методы могли захватывать lock_guard на этом объекте.
  mutable std::mutex mu_;
  std::unordered_map<std::string, UserBalances> balances_;     // user -> currency -> Balance
  std::unordered_map<std::string, Reservation> reservations_;  // reservation_id -> Reservation

  // Возвращает (создавая при необходимости) запись Balance для пары (user, ccy).
  // ВАЖНО: вызывается ТОЛЬКО под захваченным mu_.
  Balance& ensure_balance_locked(const std::string& user, const std::string& currency);
};

}  // namespace cex::ledger::app
