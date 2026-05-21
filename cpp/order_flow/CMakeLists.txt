cmake_minimum_required(VERSION 3.24)
project(order_flow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(order_flow
  src/main.cpp
  src/transport/grpc_order_flow_service.cpp
  src/app/order_flow_uc.cpp
  src/infra/risk_client.cpp
  src/infra/ledger_client.cpp
  src/infra/orders_kafka_publisher.cpp
)

target_include_directories(order_flow PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(order_flow PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(order_flow PRIVATE -Wall -Wextra -Wpedantic)
