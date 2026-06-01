cmake_minimum_required(VERSION 3.24)
project(order_flow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PR-F02-001: order_flow now persists accepted FlowOrders to flow_orders +
# flow_order_legs so the F-04 matching loop's PG reader picks them up.
# libpqxx integration mirrors cpp/matching/CMakeLists.txt — first try the
# CMake config-installed target, then fall back to pkg-config.
find_package(PostgreSQL REQUIRED)
find_package(libpqxx CONFIG QUIET)
if(libpqxx_FOUND)
  if(TARGET libpqxx::pqxx)
    set(ORDER_FLOW_PQXX_TARGET libpqxx::pqxx)
  elseif(TARGET pqxx)
    set(ORDER_FLOW_PQXX_TARGET pqxx)
  else()
    message(FATAL_ERROR "libpqxx was found but no known CMake target is available")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
  set(ORDER_FLOW_PQXX_TARGET PkgConfig::LIBPQXX)
endif()

add_executable(order_flow
  src/main.cpp
  src/transport/grpc_order_flow_service.cpp
  src/app/order_flow_uc.cpp
  src/infra/risk_client.cpp
  src/infra/ledger_client.cpp
  src/infra/orders_kafka_publisher.cpp
  src/infra/batch_results_consumer.cpp
  src/infra/postgres/postgres_flow_order_repository.cpp
)

target_include_directories(order_flow PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(order_flow PRIVATE
  cex_common
  contracts_proto
  ${ORDER_FLOW_PQXX_TARGET}
  PostgreSQL::PostgreSQL
)

target_compile_options(order_flow PRIVATE -Wall -Wextra -Wpedantic)
