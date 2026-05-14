# =============================================================================
# Сборка сервиса `ledger` — учёта балансов пользователей.
#
# Ответственности:
#   * хранить балансы (available/reserved) по пользователю и валюте;
#   * резервировать и освобождать средства под ордера (ReserveFunds/ReleaseFunds);
#   * применять результаты матчинга (ApplyBatchResult) — изменять балансы
#     по событиям из топика batch.outputs;
#   * принимать execution-репорты с внешних venue (хедж-учёт — TODO в MVP).
# Транспорт: gRPC сервер LedgerService + два Kafka-консьюмера для async-потоков.
# =============================================================================

cmake_minimum_required(VERSION 3.22)
project(ledger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ledger
  src/main.cpp                            # entrypoint: gRPC server + Kafka consumers
  src/transport/grpc_ledger_service.cpp   # gRPC-обёртка над use-case'ами
  src/app/ledger_uc.cpp                   # use-cases: бизнес-логика балансов и резервов
  src/infra/kafka_consumers.cpp           # фоновые консьюмеры batch.outputs / execution.reports
)

target_include_directories(ledger PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ledger PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(ledger PRIVATE -Wall -Wextra -Wpedantic)
