#pragma once
// =============================================================================
// OrderFlowUseCases — application-слой order_flow.
// Оркестрирует три внешних компонента: Risk, Ledger и Kafka-публикатор.
//
// Жизненный цикл ордера:
//   Create  -> validate -> Risk.CheckNewOrder -> Ledger.ReserveFunds -> Kafka.publish(create)
//   Cancel  -> Kafka.publish(cancel) + Ledger.ReleaseFunds
//   Get     -> чтение из in-memory orders_
//
// orders_ — упрощённое in-memory хранилище для MVP. В проде нужно либо
// перенести в Postgres (или Aerospike/Redis), либо строить полное
// event-sourced состояние из Kafka.
// =============================================================================

#include <unordered_map>

#include "cex/common/decimal.hpp"
#include "fob/orders/v1/order_flow_service.pb.h"

#include "infra/risk_client.hpp"
#include "infra/ledger_client.hpp"
#include "infra/orders_kafka_publisher.hpp"

namespace cex::order_flow::app {

class OrderFlowUseCases {
 public:
  // Принимает клиентов value-by-move, чтобы владеть жизненным циклом.
  OrderFlowUseCases(infra::RiskClient risk,
                    infra::LedgerClient ledger,
                    infra::OrdersKafkaPublisher publisher);

  // Создание flow-ордера с предварительными проверками и резервом средств.
  fob::orders::v1::CreateFlowOrderResponse CreateFlowOrder(
      const fob::orders::v1::CreateFlowOrderRequest& req);

  // Отмена ордера: публикуем событие в Kafka и снимаем резерв.
  fob::orders::v1::CancelFlowOrderResponse CancelFlowOrder(
      const fob::orders::v1::CancelFlowOrderRequest& req);

  // Чтение текущего состояния ордера (только то, что в нашем кэше).
  fob::orders::v1::GetFlowOrderResponse GetFlowOrder(
      const fob::orders::v1::GetFlowOrderRequest& req);

 private:
  infra::RiskClient risk_;
  infra::LedgerClient ledger_;
  infra::OrdersKafkaPublisher publisher_;

  // In-memory кэш ордеров (MVP). Не потокобезопасный — gRPC сервер ходит в один use-case
  // последовательно (до тех пор, пока мы не подключим многопоточный server thread pool).
  // При расширении необходимо добавить мьютекс или перевести на потокобезопасную структуру.
  std::unordered_map<std::string, fob::orders::v1::FlowOrder> orders_;
};

}  // namespace cex::order_flow::app
