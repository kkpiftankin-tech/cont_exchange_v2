#pragma once
// =============================================================================
// LedgerClient — обёртка над gRPC-stub'ом для сервиса ledger.
// Кэширует stub_ один раз на весь сервис, переиспользует канал.
// При gRPC-ошибках возвращает ответ с success=false / error.code="GRPC_ERROR",
// чтобы вызывающий код не работал с сетевыми сбоями отдельно от бизнес-ошибок.
// =============================================================================

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fob/ledger/v1/ledger.grpc.pb.h"

namespace cex::order_flow::infra {

class LedgerClient {
 public:
  // target — host:port адрес ledger-сервиса (например, "ledger:50053").
  explicit LedgerClient(const std::string& target);

  // Резерв средств. Идемпотентно по reservation_id на стороне ledger'а.
  fob::ledger::v1::ReserveFundsResponse ReserveFunds(
      const fob::ledger::v1::ReserveFundsRequest& req);

  // Снять резерв. void — нам не нужны детали ответа на этом уровне.
  void ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req);

  // Получить балансы пользователя — используется gateway'ем для отдачи UI.
  fob::ledger::v1::GetBalancesResponse GetBalances(
      const fob::ledger::v1::GetBalancesRequest& req);

 private:
  std::unique_ptr<fob::ledger::v1::LedgerService::Stub> stub_;
};

}  // namespace cex::order_flow::infra
