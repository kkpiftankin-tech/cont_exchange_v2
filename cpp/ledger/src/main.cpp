// =============================================================================
// Entrypoint сервиса ledger.
//
// Поднимает три рантайм-компонента:
//   1. LedgerUseCases — in-memory хранилище балансов и логика операций;
//   2. KafkaConsumers — фоновые потоки для batch.outputs и execution.reports;
//   3. gRPC-сервер LedgerService — синхронный API для других сервисов
//      (order_flow зовёт ReserveFunds/ReleaseFunds, gateway — GetBalances).
// =============================================================================

#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/ledger_uc.hpp"
#include "infra/kafka_consumers.hpp"
#include "transport/grpc_ledger_service.hpp"

int main() {
  // Адрес gRPC-сервера ledger; по умолчанию слушаем на всех интерфейсах:50053.
  const std::string listen_addr =
      cex::common::Env::get_string("LEDGER_GRPC_LISTEN", "0.0.0.0:50053");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // Use-cases — общий объект для gRPC и Kafka-консьюмеров; внутри своя мьютекс-защита.
  cex::ledger::app::LedgerUseCases uc;

  // Поднимаем фоновых консьюмеров (топики batch.outputs, execution.reports).
  cex::ledger::infra::KafkaConsumers consumers(&uc, brokers);
  consumers.start();

  // Регистрируем gRPC сервис, который проксирует вызовы в LedgerUseCases.
  cex::ledger::transport::GrpcLedgerService svc(&uc);

  grpc::ServerBuilder builder;
  // InsecureServerCredentials — для dev. В проде нужен mTLS.
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "Ledger gRPC listening", {{"addr", listen_addr}});
  // Wait блокирует до Shutdown сервера. Сейчас Shutdown не зовётся —
  // процесс просто получит SIGTERM и упадёт целиком.
  server->Wait();

  consumers.stop();
  return 0;
}
