cmake_minimum_required(VERSION 3.24)
project(ledger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ledger
  src/main.cpp
  src/transport/grpc_ledger_service.cpp
  src/app/ledger_uc.cpp
  src/infra/kafka_consumers.cpp
)

target_include_directories(ledger PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ledger PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(ledger PRIVATE -Wall -Wextra -Wpedantic)
