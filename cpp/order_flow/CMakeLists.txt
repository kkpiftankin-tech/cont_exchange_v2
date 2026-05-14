# =============================================================================
# Сборка сервиса `order_flow` — точки входа клиентских команд по ордерам.
#
# Это application-сервис: пользовательский запрос (через gateway или прямой
# gRPC) сначала валидируется, потом проходит риск-чек (gRPC к risk),
# затем резервируются средства (gRPC к ledger), и только потом ордер
# публикуется в Kafka (orders.normalized) для матчинга.
# =============================================================================

cmake_minimum_required(VERSION 3.22)
project(order_flow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(order_flow
  src/main.cpp                                   # entrypoint: gRPC server
  src/transport/grpc_order_flow_service.cpp      # gRPC API
  src/app/order_flow_uc.cpp                      # use-cases (создание/отмена/чтение)
  src/infra/risk_client.cpp                      # gRPC-клиент к сервису risk
  src/infra/ledger_client.cpp                    # gRPC-клиент к сервису ledger
  src/infra/orders_kafka_publisher.cpp           # публикатор orders.normalized в Kafka
)

target_include_directories(order_flow PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(order_flow PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(order_flow PRIVATE -Wall -Wextra -Wpedantic)
