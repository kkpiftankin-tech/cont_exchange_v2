#include <grpcpp/grpcpp.h>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/ledger_uc.hpp"
#include "infra/kafka_consumers.hpp"
#include "transport/grpc_ledger_service.hpp"

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("LEDGER_GRPC_LISTEN", "0.0.0.0:50053");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  cex::ledger::app::LedgerUseCases uc;

  // Start Kafka consumers in background (batch.outputs, execution.reports)
  cex::ledger::infra::KafkaConsumers consumers(&uc, brokers);
  consumers.start();

  cex::ledger::transport::GrpcLedgerService svc(&uc);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  auto server = builder.BuildAndStart();
  cex::common::log_json("INFO", "Ledger gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  consumers.stop();
  return 0;
}
